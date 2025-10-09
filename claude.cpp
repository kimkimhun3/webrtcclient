#define GST_USE_UNSTABLE_API

#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <iostream>
#include <getopt.h>

struct Config {
    gchar *codec;
    gint bitrate;
    gint fps;
    gint width;
    gint height;
    gchar *device;
    gchar *adev;
    gchar *server_url;
};

static GstElement *pipeline = NULL;
static GstElement *webrtc = NULL;
static SoupWebsocketConnection *ws_conn = NULL;
static GMainLoop *loop = NULL;
static gchar *peer_id = NULL;
static gchar *my_id = NULL;
static struct Config config;
static gboolean offer_in_progress = FALSE;
static gboolean connection_active = FALSE;
static guint connection_timeout_id = 0;
static GMutex webrtc_mutex;
static gboolean is_destroying = FALSE;
static gulong sig_ice_candidate = 0;
static gulong sig_negotiation = 0;
static gulong sig_ice_gathering = 0;
static gulong sig_ice_connection = 0;
static gulong sig_pad_added = 0;

// Function declarations
static void on_offer_created(GstPromise *promise, gpointer user_data);
static void force_renegotiate();
static void on_negotiation_needed(GstElement *element, gpointer user_data);
static void on_ice_candidate(GstElement *webrtc, guint mlineindex, gchar *candidate, gpointer user_data);
static void send_ice_candidate_message(guint mlineindex, const gchar *candidate);
static void on_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data);
static void on_ice_gathering_state_notify(GstElement *webrtc, GParamSpec *pspec, gpointer user_data);
static void on_ice_connection_state_notify(GstElement *webrtc, GParamSpec *pspec, gpointer user_data);
static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data);
static void reset_peer_state();
static std::string build_pipeline_string();
static void configure_turn_server();
static gboolean build_and_start_pipeline();
static void stop_and_destroy_pipeline();
static gboolean restart_pipeline();

static gboolean connection_timeout_handler(gpointer user_data) {
    g_print("⚠ Connection timeout - no answer received in 15 seconds\n");
    connection_timeout_id = 0;
    
    if (!connection_active) {
        g_print("Connection failed, waiting for new request...\n");
        reset_peer_state();
    }
    
    return G_SOURCE_REMOVE;
}

static void send_json_message(JsonObject *msg) {
    if (!ws_conn) {
        g_printerr("WebSocket not connected\n");
        return;
    }

    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, msg);
    
    gchar *text = json_to_string(root, FALSE);
    g_print("→ Sending: %s\n", text);
    
    soup_websocket_connection_send_text(ws_conn, text);
    
    g_free(text);
    json_node_free(root);
}

static void reset_peer_state() {
    g_print("Resetting peer state\n");
    
    if (connection_timeout_id != 0) {
        g_source_remove(connection_timeout_id);
        connection_timeout_id = 0;
    }
    
    connection_active = FALSE;
    
    if (peer_id) {
        g_free(peer_id);
        peer_id = NULL;
    }
    
    offer_in_progress = FALSE;
}

static std::string build_pipeline_string() {
    const char *encoder, *parser, *payloader, *encoding_name;
    int payload = 96;

    if (g_strcmp0(config.codec, "h265") == 0) {
        encoder = "omxh265enc";
        parser  = "h265parse";
        payloader = "rtph265pay";
        encoding_name = "H265";
    } else {
        encoder = "omxh264enc";
        parser  = "h264parse";
        payloader = "rtph264pay";
        encoding_name = "H264";
    }

    char pipeline_buf[4096];
    snprintf(pipeline_buf, sizeof(pipeline_buf),
        "webrtcbin name=webrtcbin bundle-policy=max-bundle latency=30 "
        "stun-server=stun://stun.relay.metered.ca:80 "
        "v4l2src device=%s ! "
        "video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
        "videoconvert ! "
        "queue max-size-buffers=3 leaky=downstream ! "
        "%s target-bitrate=%d control-rate=2 ! "
        "%s ! "
        "%s config-interval=1 pt=%d ! "
        "application/x-rtp,media=video,encoding-name=%s,payload=%d ! "
        "webrtcbin. "
        "alsasrc device=%s provide-clock=false do-timestamp=true buffer-time=200000 latency-time=10000 ! "
        "audio/x-raw,rate=48000,channels=2,format=S16LE ! "
        "audioconvert ! audioresample ! "
        "queue max-size-time=200000000 max-size-buffers=0 leaky=downstream ! "
        "opusenc bitrate=96000 frame-size=20 complexity=5 inband-fec=true dtx=false ! "
        "rtpopuspay pt=97 ! "
        "application/x-rtp,media=audio,encoding-name=OPUS,payload=97 ! "
        "webrtcbin.",
        config.device, config.width, config.height, config.fps,
        encoder, config.bitrate,
        parser,
        payloader, payload, encoding_name, payload,
        config.adev
    );

    g_print("\n╔═══ Configuration ═══╗\n");
    g_print("Codec:      %s\n", config.codec);
    g_print("Resolution: %dx%d\n", config.width, config.height);
    g_print("Framerate:  %d fps\n", config.fps);
    g_print("Bitrate:    %d kbps\n", config.bitrate);
    g_print("Device:     %s\n", config.device);
    g_print("ALSA dev:   %s\n", config.adev);
    g_print("Server:     %s\n", config.server_url);
    g_print("╚═════════════════════╝\n\n");

    return std::string(pipeline_buf);
}

static void configure_turn_server() {
    g_mutex_lock(&webrtc_mutex);
    
    if (!webrtc || !GST_IS_ELEMENT(webrtc)) {
        g_printerr("Cannot configure TURN: webrtc element not available\n");
        g_mutex_unlock(&webrtc_mutex);
        return;
    }
    
    g_print("Configuring TURN servers...\n");
    
    const char* turn_servers[] = {
        "turn://0f88d20baa787ce808206382:IEBAfahDFQ0Nk9V1@global.relay.metered.ca:80",
        "turn://0f88d20baa787ce808206382:IEBAfahDFQ0Nk9V1@global.relay.metered.ca:80?transport=tcp",
        "turn://0f88d20baa787ce808206382:IEBAfahDFQ0Nk9V1@global.relay.metered.ca:443",
        "turns://0f88d20baa787ce808206382:IEBAfahDFQ0Nk9V1@global.relay.metered.ca:443?transport=tcp",
        NULL
    };
    
    for (int i = 0; turn_servers[i] != NULL; i++) {
        gboolean ret = FALSE;
        g_signal_emit_by_name(webrtc, "add-turn-server", turn_servers[i], &ret);
        
        if (ret) {
            g_print("✓ Added TURN server: %s\n", turn_servers[i]);
        } else {
            g_printerr("✗ Failed to add TURN server: %s\n", turn_servers[i]);
        }
    }
    
    g_mutex_unlock(&webrtc_mutex);
    g_print("\n");
}

static void disconnect_webrtc_signals() {
    g_mutex_lock(&webrtc_mutex);
    
    if (webrtc && GST_IS_ELEMENT(webrtc)) {
        if (sig_ice_candidate != 0) {
            g_signal_handler_disconnect(webrtc, sig_ice_candidate);
            sig_ice_candidate = 0;
        }
        if (sig_negotiation != 0) {
            g_signal_handler_disconnect(webrtc, sig_negotiation);
            sig_negotiation = 0;
        }
        if (sig_ice_gathering != 0) {
            g_signal_handler_disconnect(webrtc, sig_ice_gathering);
            sig_ice_gathering = 0;
        }
        if (sig_ice_connection != 0) {
            g_signal_handler_disconnect(webrtc, sig_ice_connection);
            sig_ice_connection = 0;
        }
        if (sig_pad_added != 0) {
            g_signal_handler_disconnect(webrtc, sig_pad_added);
            sig_pad_added = 0;
        }
    }
    
    g_mutex_unlock(&webrtc_mutex);
}

static gboolean build_and_start_pipeline() {
    GError *error = NULL;
    std::string pipeline_str = build_pipeline_string();

    pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
    if (error) {
        g_printerr("Failed to create pipeline: %s\n", error->message);
        g_error_free(error);
        pipeline = NULL;
        return FALSE;
    }

    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtcbin");
    if (!webrtc) {
        g_printerr("webrtcbin not found in pipeline\n");
        gst_object_unref(pipeline);
        pipeline = NULL;
        return FALSE;
    }

    // Keep an extra reference to prevent premature destruction
    gst_object_ref(webrtc);

    configure_turn_server();

    sig_negotiation = g_signal_connect(webrtc, "on-negotiation-needed", 
                                        G_CALLBACK(on_negotiation_needed), NULL);
    sig_ice_candidate = g_signal_connect(webrtc, "on-ice-candidate", 
                                         G_CALLBACK(on_ice_candidate), NULL);
    sig_pad_added = g_signal_connect(webrtc, "pad-added", 
                                     G_CALLBACK(on_incoming_stream), NULL);
    sig_ice_gathering = g_signal_connect(webrtc, "notify::ice-gathering-state",
                                        G_CALLBACK(on_ice_gathering_state_notify), NULL);
    sig_ice_connection = g_signal_connect(webrtc, "notify::ice-connection-state",
                                         G_CALLBACK(on_ice_connection_state_notify), NULL);

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_print("✓ Pipeline started\n");
    return TRUE;
}

static void stop_and_destroy_pipeline() {
    if (!pipeline) return;
    
    g_print("Stopping pipeline...\n");
    
    g_mutex_lock(&webrtc_mutex);
    is_destroying = TRUE;
    g_mutex_unlock(&webrtc_mutex);
    
    if (connection_timeout_id != 0) {
        g_source_remove(connection_timeout_id);
        connection_timeout_id = 0;
    }
    
    connection_active = FALSE;
    
    // Disconnect all signals first
    disconnect_webrtc_signals();
    
    // Stop the pipeline
    gst_element_set_state(pipeline, GST_STATE_NULL);
    
    // Wait for state change
    GstState state;
    gst_element_get_state(pipeline, &state, NULL, 3 * GST_SECOND);
    
    // Small delay to let any pending callbacks complete
    g_usleep(200000);  // 200ms
    
    g_mutex_lock(&webrtc_mutex);
    
    if (webrtc) {
        // Release our extra reference
        gst_object_unref(webrtc);
        // Release the reference from gst_bin_get_by_name
        gst_object_unref(webrtc);
        webrtc = NULL;
    }
    
    g_mutex_unlock(&webrtc_mutex);
    
    gst_object_unref(pipeline);
    pipeline = NULL;
    
    g_mutex_lock(&webrtc_mutex);
    is_destroying = FALSE;
    g_mutex_unlock(&webrtc_mutex);
    
    g_print("Pipeline destroyed\n");
}

static gboolean restart_pipeline() {
    stop_and_destroy_pipeline();
    g_usleep(300000);  // 300ms delay
    return build_and_start_pipeline();
}

static void on_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type,
                       GBytes *message, gpointer user_data) {
    if (type != SOUP_WEBSOCKET_DATA_TEXT) {
        return;
    }

    gsize size;
    const gchar *data = (const gchar *)g_bytes_get_data(message, &size);
    gchar *text = g_strndup(data, size);
    
    g_print("← Received: %s\n", text);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, text, -1, NULL)) {
        g_printerr("Failed to parse JSON\n");
        g_free(text);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *object = json_node_get_object(root);
    const gchar *msg_type = json_object_get_string_member(object, "type");

    if (g_strcmp0(msg_type, "registered") == 0) {
        my_id = g_strdup(json_object_get_string_member(object, "id"));
        g_print("✓ Registered with ID: %s\n", my_id);
        
    } else if (g_strcmp0(msg_type, "answer") == 0) {
        const gchar *sdp_text = json_object_get_string_member(object, "sdp");
        const gchar *from_id = json_object_get_string_member(object, "from");
        
        g_print("✓ Received answer from: %s\n", from_id);

        if (connection_timeout_id != 0) {
            g_source_remove(connection_timeout_id);
            connection_timeout_id = 0;
        }

        if (!peer_id || g_strcmp0(peer_id, from_id) != 0) {
            if (peer_id) {
                g_free(peer_id);
            }
            peer_id = g_strdup(from_id);
        }

        g_mutex_lock(&webrtc_mutex);
        gboolean webrtc_valid = (webrtc && GST_IS_ELEMENT(webrtc) && !is_destroying);
        g_mutex_unlock(&webrtc_mutex);

        if (!webrtc_valid) {
            g_printerr("Cannot set answer: webrtc element not valid\n");
            g_free(text);
            g_object_unref(parser);
            return;
        }

        GstSDPMessage *sdp;
        if (gst_sdp_message_new(&sdp) != GST_SDP_OK) {
            g_printerr("Failed to create SDP message\n");
            g_free(text);
            g_object_unref(parser);
            return;
        }
        
        if (gst_sdp_message_parse_buffer((guint8 *)sdp_text, strlen(sdp_text), sdp) != GST_SDP_OK) {
            g_printerr("Failed to parse SDP\n");
            gst_sdp_message_free(sdp);
            g_free(text);
            g_object_unref(parser);
            return;
        }

        GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(
            GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        
        GstPromise *promise = gst_promise_new();
        
        g_mutex_lock(&webrtc_mutex);
        if (webrtc && GST_IS_ELEMENT(webrtc) && !is_destroying) {
            g_signal_emit_by_name(webrtc, "set-remote-description", answer, promise);
        }
        g_mutex_unlock(&webrtc_mutex);
        
        gst_promise_interrupt(promise);
        gst_promise_unref(promise);
        
        gst_webrtc_session_description_free(answer);
        offer_in_progress = FALSE;
        connection_active = TRUE;
        
        g_print("✓ Answer set, connection establishing...\n");
        
    } else if (g_strcmp0(msg_type, "ice-candidate") == 0) {
        if (!json_object_has_member(object, "candidate")) {
            g_print("ICE candidate message missing 'candidate' field\n");
            g_free(text);
            g_object_unref(parser);
            return;
        }
        
        JsonObject *candidate_obj = json_object_get_object_member(object, "candidate");
        if (!candidate_obj) {
            g_print("Invalid candidate object\n");
            g_free(text);
            g_object_unref(parser);
            return;
        }
        
        const gchar *candidate_str = json_object_get_string_member(candidate_obj, "candidate");
        
        if (!candidate_str || strlen(candidate_str) == 0) {
            g_print("Received end-of-candidates signal, ignoring\n");
            g_free(text);
            g_object_unref(parser);
            return;
        }
        
        g_mutex_lock(&webrtc_mutex);
        gboolean webrtc_valid = (webrtc && GST_IS_ELEMENT(webrtc) && !is_destroying);
        g_mutex_unlock(&webrtc_mutex);
        
        if (!webrtc_valid) {
            g_print("Cannot add ICE candidate: webrtc element not valid\n");
            g_free(text);
            g_object_unref(parser);
            return;
        }
        
        guint sdp_mline_index = json_object_get_int_member(candidate_obj, "sdpMLineIndex");
        
        g_print("✓ Adding ICE candidate [%u]: %s\n", sdp_mline_index, candidate_str);
        
        g_mutex_lock(&webrtc_mutex);
        if (webrtc && GST_IS_ELEMENT(webrtc) && !is_destroying) {
            g_signal_emit_by_name(webrtc, "add-ice-candidate", sdp_mline_index, candidate_str);
        }
        g_mutex_unlock(&webrtc_mutex);

    } else if (g_strcmp0(msg_type, "request-offer") == 0) {
        const gchar *from_id = NULL;
        if (json_object_has_member(object, "from")) {
            from_id = json_object_get_string_member(object, "from");
        }
        
        g_print("✓ Received request-offer");
        if (from_id) {
            g_print(" from %s", from_id);
        }
        g_print("\n");
        
        reset_peer_state();
        if (from_id) {
            peer_id = g_strdup(from_id);
        }
        
        if (!restart_pipeline()) {
            g_printerr("Failed to restart pipeline\n");
            g_free(text);
            g_object_unref(parser);
            return;
        }
        
        force_renegotiate();
        
    } else if (g_strcmp0(msg_type, "peer-left") == 0) {
        const gchar *left_id = NULL;
        if (json_object_has_member(object, "id")) {
            left_id = json_object_get_string_member(object, "id");
        }
        
        g_print("Peer left notification");
        if (left_id) {
            g_print(": %s", left_id);
        }
        g_print("\n");
        
        if (left_id && peer_id && g_strcmp0(left_id, peer_id) == 0) {
            g_print("Our peer disconnected, restarting pipeline\n");
            reset_peer_state();
            restart_pipeline();
        }
    }

    g_free(text);
    g_object_unref(parser);
}

static void send_ice_candidate_message(guint mlineindex, const gchar *candidate) {
    if (!ws_conn || !candidate) {
        return;
    }
    
    JsonObject *ice = json_object_new();
    json_object_set_string_member(ice, "candidate", candidate);
    json_object_set_int_member(ice, "sdpMLineIndex", mlineindex);

    JsonObject *msg = json_object_new();
    json_object_set_string_member(msg, "type", "ice-candidate");
    json_object_set_object_member(msg, "candidate", ice);
    if (peer_id) {
        json_object_set_string_member(msg, "to", peer_id);
    }

    send_json_message(msg);
    json_object_unref(msg);
}

static void on_ice_candidate(GstElement *webrtc_elem, guint mlineindex,
                             gchar *candidate, gpointer user_data) {
    // Check destroying flag first
    g_mutex_lock(&webrtc_mutex);
    gboolean destroying = is_destroying;
    gboolean valid = (webrtc && GST_IS_ELEMENT(webrtc) && webrtc == webrtc_elem);
    g_mutex_unlock(&webrtc_mutex);
    
    if (destroying || !valid) {
        return;
    }
    
    if (!candidate || strlen(candidate) == 0) {
        g_print("ICE gathering completed\n");
        return;
    }
    
    if (!pipeline || GST_STATE(pipeline) != GST_STATE_PLAYING) {
        return;
    }
    
    if (strstr(candidate, "typ relay")) {
        g_print("📡 Generated TURN relay candidate: %s\n", candidate);
    } else if (strstr(candidate, "typ srflx")) {
        g_print("🌍 Generated STUN srflx candidate: %s\n", candidate);
    } else if (strstr(candidate, "typ host")) {
        g_print("🏠 Generated host candidate: %s\n", candidate);
    }
    
    send_ice_candidate_message(mlineindex, candidate);
}

static void force_renegotiate() {
    g_mutex_lock(&webrtc_mutex);
    
    if (!webrtc || !GST_IS_ELEMENT(webrtc) || is_destroying) {
        g_printerr("Cannot renegotiate: webrtc element not available\n");
        g_mutex_unlock(&webrtc_mutex);
        return;
    }
    
    if (offer_in_progress) {
        g_print("Offer already in progress, skipping\n");
        g_mutex_unlock(&webrtc_mutex);
        return;
    }
    
    g_print("Creating new offer for reconnection\n");
    offer_in_progress = TRUE;
    
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
    g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
    
    g_mutex_unlock(&webrtc_mutex);
}

static void on_offer_created(GstPromise *promise, gpointer user_data) {
    g_mutex_lock(&webrtc_mutex);
    gboolean destroying = is_destroying;
    g_mutex_unlock(&webrtc_mutex);
    
    if (destroying) {
        gst_promise_unref(promise);
        offer_in_progress = FALSE;
        return;
    }
    
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    
    if (!reply) {
        g_printerr("Failed to get promise reply\n");
        gst_promise_unref(promise);
        offer_in_progress = FALSE;
        return;
    }
    
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    if (!offer) {
        g_printerr("Failed to create offer\n");
        offer_in_progress = FALSE;
        return;
    }

    g_mutex_lock(&webrtc_mutex);
    gboolean webrtc_valid = (webrtc && GST_IS_ELEMENT(webrtc) && !is_destroying);
    g_mutex_unlock(&webrtc_mutex);

    if (!webrtc_valid) {
        g_printerr("webrtc element no longer valid\n");
        gst_webrtc_session_description_free(offer);
        offer_in_progress = FALSE;
        return;
    }

    g_print("✓ Offer created, setting local description\n");
    
    promise = gst_promise_new();
    
    g_mutex_lock(&webrtc_mutex);
    if (webrtc && GST_IS_ELEMENT(webrtc) && !is_destroying) {
        g_signal_emit_by_name(webrtc, "set-local-description", offer, promise);
    }
    g_mutex_unlock(&webrtc_mutex);
    
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);

    gchar *sdp_text = gst_sdp_message_as_text(offer->sdp);
    
    JsonObject *msg = json_object_new();
    json_object_set_string_member(msg, "type", "offer");
    json_object_set_string_member(msg, "sdp", sdp_text);
    if (peer_id) {
        json_object_set_string_member(msg, "to", peer_id);
    }

    send_json_message(msg);
    
    g_free(sdp_text);
    json_object_unref(msg);
    gst_webrtc_session_description_free(offer);
    
    if (connection_timeout_id != 0) {
        g_source_remove(connection_timeout_id);
    }
    connection_timeout_id = g_timeout_add_seconds(15, connection_timeout_handler, NULL);
    g_print("Started connection timeout (15s)\n");
}

static void on_negotiation_needed(GstElement *element, gpointer user_data) {
    g_mutex_lock(&webrtc_mutex);
    gboolean destroying = is_destroying;
    g_mutex_unlock(&webrtc_mutex);
    
    if (!destroying) {
        g_print("Negotiation needed signal received\n");
    }
}

static void on_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data) {
    g_mutex_lock(&webrtc_mutex);
    gboolean destroying = is_destroying;
    g_mutex_unlock(&webrtc_mutex);
    
    if (!destroying) {
        g_print("Received incoming stream (unexpected for sender)\n");
    }
}

static void on_ice_gathering_state_notify(GstElement *webrtc_elem, GParamSpec *pspec, gpointer user_data) {
    g_mutex_lock(&webrtc_mutex);
    gboolean destroying = is_destroying;
    gboolean valid = (webrtc && GST_IS_ELEMENT(webrtc) && webrtc == webrtc_elem);
    
    if (destroying || !valid) {
        g_mutex_unlock(&webrtc_mutex);
        return;
    }
    
    GstWebRTCICEGatheringState ice_gather_state;
    g_object_get(webrtc, "ice-gathering-state", &ice_gather_state, NULL);
    
    g_mutex_unlock(&webrtc_mutex);
    
    const gchar *state_str = NULL;
    switch (ice_gather_state) {
        case GST_WEBRTC_ICE_GATHERING_STATE_NEW:
            state_str = "new";
            break;
        case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:
            state_str = "gathering";
            break;
        case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:
            state_str = "complete";
            break;
        default:
            state_str = "unknown";
    }
    
    g_print("ICE gathering state: %s\n", state_str);
}

static void on_ice_connection_state_notify(GstElement *webrtc_elem, GParamSpec *pspec, gpointer user_data) {
    g_mutex_lock(&webrtc_mutex);
    gboolean destroying = is_destroying;
    gboolean valid = (webrtc && GST_IS_ELEMENT(webrtc) && webrtc == webrtc_elem);
    
    if (destroying || !valid) {
        g_mutex_unlock(&webrtc_mutex);
        return;
    }
    
    GstWebRTCICEConnectionState ice_conn_state;
    g_object_get(webrtc, "ice-connection-state", &ice_conn_state, NULL);
    
    g_mutex_unlock(&webrtc_mutex);
    
    const gchar *state_str = NULL;
    switch (ice_conn_state) {
        case GST_WEBRTC_ICE_CONNECTION_STATE_NEW:
            state_str = "new";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING:
            state_str = "checking";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:
            state_str = "connected";
            g_print("✓✓✓ ICE connection established ✓✓✓\n");
            connection_active = TRUE;
            if (connection_timeout_id != 0) {
                g_source_remove(connection_timeout_id);
                connection_timeout_id = 0;
            }
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED:
            state_str = "completed";
            connection_active = TRUE;
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:
            state_str = "failed";
            g_printerr("✗ ICE connection failed\n");
            connection_active = FALSE;
            reset_peer_state();
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED:
            state_str = "disconnected";
            g_print("Peer disconnected\n");
            connection_active = FALSE;
            reset_peer_state();
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED:
            state_str = "closed";
            connection_active = FALSE;
            reset_peer_state();
            break;
        default:
            state_str = "unknown";
    }
    
    g_print("ICE connection state: %s\n", state_str);
}

static void on_websocket_connected(GObject *session, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    ws_conn = soup_session_websocket_connect_finish(SOUP_SESSION(session), res, &error);
    
    if (error) {
        g_printerr("✗ WebSocket connection failed: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }

    g_print("✓✓✓ WebSocket connected to signaling server ✓✓✓\n");
    
    g_signal_connect(ws_conn, "message", G_CALLBACK(on_message), NULL);
    
    g_signal_connect(ws_conn, "closed", G_CALLBACK(+[](SoupWebsocketConnection *conn, gpointer data) {
        g_print("WebSocket closed\n");
        g_main_loop_quit((GMainLoop*)data);
    }), loop);
}

static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data) {
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;
            gst_message_parse_error(message, &err, &debug);
            g_printerr("✗ Error: %s\n", err->message);
            g_printerr("Debug: %s\n", debug);
            g_error_free(err);
            g_free(debug);
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err;
            gchar *debug;
            gst_message_parse_warning(message, &err, &debug);
            g_printerr("⚠ Warning: %s\n", err->message);
            g_error_free(err);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(loop);
            break;
        default:
            break;
    }
    return TRUE;
}

static void print_usage(const char *prog_name) {
    g_print("Usage: %s [OPTIONS]\n", prog_name);
    g_print("\nOptions:\n");
    g_print("  --codec=CODEC       Video codec: h264 or h265 (default: h264)\n");
    g_print("  --bitrate=KBPS      Video bitrate in kbps (default: 2000)\n");
    g_print("  --fps=FPS           Framerate (default: 30)\n");
    g_print("  --width=WIDTH       Video width (default: 1280)\n");
    g_print("  --height=HEIGHT     Video height (default: 720)\n");
    g_print("  --device=PATH       Camera device path (default: /dev/video0)\n");
    g_print("  --adev=ALSA         ALSA audio device (default: hw:1,1)\n");
    g_print("  --server=URL        Signaling server URL (default: ws://localhost:8080/ws)\n");
    g_print("  --help              Show this help message\n");
}

static gboolean parse_arguments(int argc, char *argv[]) {
    config.codec = g_strdup("h264");
    config.bitrate = 2000;
    config.fps = 30;
    config.width = 1280;
    config.height = 720;
    config.device = g_strdup("/dev/video0");
    config.adev = g_strdup("hw:1,1");
    config.server_url = g_strdup("ws://192.168.25.90:8080/ws");

    struct option long_options[] = {
        {"codec",    required_argument, 0, 'c'},
        {"bitrate",  required_argument, 0, 'b'},
        {"fps",      required_argument, 0, 'f'},
        {"width",    required_argument, 0, 'w'},
        {"height",   required_argument, 0, 'H'},
        {"device",   required_argument, 0, 'd'},
        {"adev",     required_argument, 0, 'a'},
        {"server",   required_argument, 0, 's'},
        {"help",     no_argument,       0, '?'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;

    while ((c = getopt_long(argc, argv, "c:b:f:w:H:d:a:s:?", long_options, &option_index)) != -1) {
        switch (c) {
            case 'c':
                g_free(config.codec);
                config.codec = g_strdup(optarg);
                if (g_strcmp0(config.codec, "h264") != 0 && g_strcmp0(config.codec, "h265") != 0) {
                    g_printerr("Error: codec must be 'h264' or 'h265'\n");
                    return FALSE;
                }
                break;
            case 'b':
                config.bitrate = atoi(optarg);
                if (config.bitrate <= 0) {
                    g_printerr("Error: bitrate must be positive\n");
                    return FALSE;
                }
                break;
            case 'f':
                config.fps = atoi(optarg);
                if (config.fps <= 0 || config.fps > 120) {
                    g_printerr("Error: fps must be between 1 and 120\n");
                    return FALSE;
                }
                break;
            case 'w':
                config.width = atoi(optarg);
                if (config.width <= 0) {
                    g_printerr("Error: width must be positive\n");
                    return FALSE;
                }
                break;
            case 'H':
                config.height = atoi(optarg);
                if (config.height <= 0) {
                    g_printerr("Error: height must be positive\n");
                    return FALSE;
                }
                break;
            case 'd':
                g_free(config.device);
                config.device = g_strdup(optarg);
                break;
            case 'a':
                g_free(config.adev);
                config.adev = g_strdup(optarg);
                break;
            case 's':
                g_free(config.server_url);
                config.server_url = g_strdup(optarg);
                break;
            case '?':
            default:
                print_usage(argv[0]);
                return FALSE;
        }
    }

    return TRUE;
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    g_mutex_init(&webrtc_mutex);

    if (!parse_arguments(argc, argv)) {
        g_mutex_clear(&webrtc_mutex);
        return -1;
    }

    loop = g_main_loop_new(NULL, FALSE);

    if (!build_and_start_pipeline()) {
        g_free(config.codec);
        g_free(config.device);
        g_free(config.adev);
        g_free(config.server_url);
        g_mutex_clear(&webrtc_mutex);
        g_main_loop_unref(loop);
        return -1;
    }

    g_print("Connecting to signaling server: %s\n", config.server_url);
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", config.server_url);
    
    soup_session_websocket_connect_async(session, msg, NULL, NULL, NULL, 
                                         on_websocket_connected, NULL);

    g_main_loop_run(loop);

    g_print("Cleaning up...\n");
    stop_and_destroy_pipeline();
    
    if (ws_conn) {
        soup_websocket_connection_close(ws_conn, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
        g_object_unref(ws_conn);
    }
    
    g_object_unref(session);
    g_main_loop_unref(loop);
    
    g_free(my_id);
    g_free(peer_id);
    g_free(config.codec);
    g_free(config.device);
    g_free(config.adev);
    g_free(config.server_url);
    
    g_mutex_clear(&webrtc_mutex);

    return 0;
}
