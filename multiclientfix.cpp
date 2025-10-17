// Multi-Client Adaptive WebRTC Streaming Server (LAN + Internet Support)
// FIXED: Robust connection/disconnection handling with proper cleanup
// Build: g++ -std=c++17 -o webrtc_multicast webrtc_multicast.cpp \
//        `pkg-config --cflags --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 \
//        libsoup-2.4 json-glib-1.0 glib-2.0 gio-2.0`

#define GST_USE_UNSTABLE_API

#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <iostream>
#include <getopt.h>
#include <map>
#include <string>
#include <time.h>
#include <queue>
#include <mutex>

// ==================== Configuration ====================
struct Config {
    gchar *codec;
    gint bitrate;
    gint fps;
    gint width;
    gint height;
    gchar *device;
    gchar *adev;
    guint port;
    gchar *www_root;
};

struct IceCandidate {
    guint mlineindex;
    std::string candidate;
};

struct PeerState {
    std::string peer_id;
    gboolean use_internet_mode;
    gboolean offer_in_progress;
    gboolean remote_description_set;
    gboolean is_cleaning_up;
    std::queue<IceCandidate> pending_ice_candidates;
    GstElement *webrtc;
    GstElement *video_queue;
    GstElement *audio_queue;
    GstPad *video_tee_pad;
    GstPad *audio_tee_pad;
    gulong negotiation_handler;
    gulong ice_candidate_handler;
    gulong ice_gathering_handler;
    gulong ice_connection_handler;
    
    PeerState() : use_internet_mode(FALSE), offer_in_progress(FALSE), 
                  remote_description_set(FALSE), is_cleaning_up(FALSE),
                  webrtc(NULL), video_queue(NULL), audio_queue(NULL),
                  video_tee_pad(NULL), audio_tee_pad(NULL),
                  negotiation_handler(0), ice_candidate_handler(0),
                  ice_gathering_handler(0), ice_connection_handler(0) {}
};

// ==================== Global Variables ====================
static SoupServer *http_server = NULL;
static std::map<std::string, SoupWebsocketConnection*> remote_clients;
static GstElement *pipeline = NULL;
static GstElement *video_tee = NULL;
static GstElement *audio_tee = NULL;
static std::map<std::string, PeerState> peers;
static std::mutex peers_mutex;
static GMainLoop *loop = NULL;
static gchar *sender_id = NULL;
static struct Config config;

// ==================== Utility Functions ====================

static std::string make_id() {
    static const char* alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string out;
    for (int i = 0; i < 9; i++) out += alphabet[rand() % 36];
    return out;
}

static void send_to_client(const std::string& client_id, const gchar* msg_text) {
    auto it = remote_clients.find(client_id);
    if (it != remote_clients.end() &&
        soup_websocket_connection_get_state(it->second) == SOUP_WEBSOCKET_STATE_OPEN) {
        soup_websocket_connection_send_text(it->second, msg_text);
    }
}

static gboolean is_rfc1918_ip(const gchar* candidate) {
    const gchar* ip_start = strstr(candidate, " ");
    if (!ip_start) return FALSE;
    
    for (int i = 0; i < 4 && ip_start; i++) {
        while (*ip_start == ' ') ip_start++;
        while (*ip_start && *ip_start != ' ') ip_start++;
    }
    while (*ip_start == ' ') ip_start++;
    
    if (g_str_has_prefix(ip_start, "192.168.")) return TRUE;
    if (g_str_has_prefix(ip_start, "10.")) return TRUE;
    if (g_str_has_prefix(ip_start, "172.16.")) return TRUE;
    for (int i = 17; i <= 31; i++) {
        gchar prefix[16];
        g_snprintf(prefix, sizeof(prefix), "172.%d.", i);
        if (g_str_has_prefix(ip_start, prefix)) return TRUE;
    }
    return FALSE;
}

static const char* guess_mime(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "text/plain";
    if (!g_ascii_strcasecmp(ext, ".html") || !g_ascii_strcasecmp(ext, ".htm")) return "text/html; charset=utf-8";
    if (!g_ascii_strcasecmp(ext, ".js"))   return "application/javascript; charset=utf-8";
    if (!g_ascii_strcasecmp(ext, ".mjs"))  return "application/javascript; charset=utf-8";
    if (!g_ascii_strcasecmp(ext, ".css"))  return "text/css; charset=utf-8";
    if (!g_ascii_strcasecmp(ext, ".json")) return "application/json; charset=utf-8";
    if (!g_ascii_strcasecmp(ext, ".png"))  return "image/png";
    if (!g_ascii_strcasecmp(ext, ".jpg") || !g_ascii_strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!g_ascii_strcasecmp(ext, ".gif"))  return "image/gif";
    if (!g_ascii_strcasecmp(ext, ".svg"))  return "image/svg+xml";
    if (!g_ascii_strcasecmp(ext, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

// ==================== HTTP Handler ====================

static void static_handler(SoupServer* server, SoupMessage* msg,
                           const char* path, GHashTable* query,
                           SoupClientContext* client, gpointer user_data)
{
    (void)server; (void)query; (void)client; (void)user_data;

    if (msg->method != SOUP_METHOD_GET && msg->method != SOUP_METHOD_HEAD) {
        soup_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED);
        return;
    }

    if (strstr(path, "..")) {
        soup_message_set_status(msg, SOUP_STATUS_FORBIDDEN);
        soup_message_set_response(msg, "text/plain", SOUP_MEMORY_COPY, "Forbidden", 9);
        return;
    }

    std::string req_path = path;
    if (req_path.empty() || req_path == "/") req_path = "/index.html";

    const char* rel_path = req_path.c_str();
    if (rel_path[0] == '/') rel_path++;

    gchar* filepath = g_build_filename(config.www_root, rel_path, NULL);
    GFile* file = g_file_new_for_path(filepath);

    gchar* contents = NULL;
    gsize len = 0;
    GError* err = NULL;

    if (g_file_load_contents(file, NULL, &contents, &len, NULL, &err)) {
        const char* mime = guess_mime(filepath);
        soup_message_set_response(msg, mime,
                                  (msg->method == SOUP_METHOD_HEAD) ? SOUP_MEMORY_COPY : SOUP_MEMORY_TAKE,
                                  contents, len);
        soup_message_set_status(msg, SOUP_STATUS_OK);
        soup_message_headers_replace(msg->response_headers, "Cache-Control", "no-cache");
    } else {
        const char* not_found_msg = "404 - File Not Found";
        soup_message_set_response(msg, "text/plain", SOUP_MEMORY_COPY,
                                  not_found_msg, strlen(not_found_msg));
        soup_message_set_status(msg, SOUP_STATUS_NOT_FOUND);
        g_clear_error(&err);
    }

    g_object_unref(file);
    g_free(filepath);
}

// ==================== WebRTC Implementation ====================

static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data);
static void send_ice_candidate_to_peer(const std::string& peer_id, guint mlineindex, const gchar *candidate);
static void on_offer_created(GstPromise *promise, gpointer user_data);
static void on_negotiation_needed(GstElement *element, gpointer user_data);
static void on_ice_candidate(GstElement *webrtc, guint mlineindex, gchar *candidate, gpointer user_data);
static void on_ice_gathering_state_notify(GstElement *webrtc, GParamSpec *pspec, gpointer user_data);
static void on_ice_connection_state_notify(GstElement *webrtc, GParamSpec *pspec, gpointer user_data);

static gboolean build_base_pipeline() {
    if (pipeline) return TRUE;
    
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

    char pipeline_str[8192];
    snprintf(pipeline_str, sizeof(pipeline_str),
        "v4l2src device=%s ! "
        "video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
        "videoconvert ! "
        "queue max-size-buffers=2 leaky=downstream ! "
        "%s target-bitrate=%d control-rate=2 ! "
        "%s ! "
        "%s config-interval=1 pt=%d ! "
        "application/x-rtp,media=video,encoding-name=%s,payload=%d ! "
        "tee name=video_tee allow-not-linked=true "
        
        "alsasrc device=%s ! "
        "audio/x-raw,rate=48000,channels=2,format=S16LE ! "
        "audioconvert ! audioresample ! "
        "queue max-size-buffers=10 leaky=downstream ! "
        "opusenc bitrate=96000 frame-size=20 complexity=5 inband-fec=true ! "
        "rtpopuspay pt=97 ! "
        "application/x-rtp,media=audio,encoding-name=OPUS,payload=97 ! "
        "tee name=audio_tee allow-not-linked=true",
        
        config.device, config.width, config.height, config.fps,
        encoder, config.bitrate * 1000,
        parser,
        payloader, payload, encoding_name, payload,
        config.adev
    );

    GError *error = NULL;
    pipeline = gst_parse_launch(pipeline_str, &error);
    if (error) {
        g_printerr("[Server] Failed to create base pipeline: %s\n", error->message);
        g_error_free(error);
        return FALSE;
    }

    video_tee = gst_bin_get_by_name(GST_BIN(pipeline), "video_tee");
    audio_tee = gst_bin_get_by_name(GST_BIN(pipeline), "audio_tee");

    if (!video_tee || !audio_tee) {
        g_printerr("[Server] Failed to get tee elements\n");
        gst_object_unref(pipeline);
        pipeline = NULL;
        return FALSE;
    }

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_print("[Server] ✓ Base pipeline created and started\n");
    return TRUE;
}

static GstElement* add_webrtc_peer(const std::string& peer_id, gboolean use_internet_mode) {
    if (!pipeline || !video_tee || !audio_tee) {
        g_printerr("[Server] Base pipeline not ready\n");
        return NULL;
    }

    GstElement *webrtc = gst_element_factory_make("webrtcbin", NULL);
    if (!webrtc) {
        g_printerr("[Server] Failed to create webrtcbin\n");
        return NULL;
    }

    if (use_internet_mode) {
        g_object_set(webrtc,
            "stun-server", "stun://stun.relay.metered.ca:80",
            "turn-server", "turn://7321ff60cbe4cad66abfbac7:af44V11U4JE4axiV@global.relay.metered.ca:80",
            NULL);
    }
    
    g_object_set(webrtc, 
        "bundle-policy", 3,
        NULL);

    gst_bin_add(GST_BIN(pipeline), webrtc);

    GstElement *video_queue = gst_element_factory_make("queue", NULL);
    GstElement *audio_queue = gst_element_factory_make("queue", NULL);
    
    g_object_set(video_queue, 
        "max-size-buffers", 0, 
        "max-size-time", G_GUINT64_CONSTANT(0), 
        "max-size-bytes", 0,
        "leaky", 2,
        NULL);
    g_object_set(audio_queue, 
        "max-size-buffers", 0, 
        "max-size-time", G_GUINT64_CONSTANT(0), 
        "max-size-bytes", 0,
        "leaky", 2,
        NULL);

    gst_bin_add_many(GST_BIN(pipeline), video_queue, audio_queue, NULL);

    GstPad *tee_video_pad = gst_element_get_request_pad(video_tee, "src_%u");
    GstPad *queue_video_sink = gst_element_get_static_pad(video_queue, "sink");
    if (gst_pad_link(tee_video_pad, queue_video_sink) != GST_PAD_LINK_OK) {
        g_printerr("[Server] Failed to link video tee to queue\n");
        gst_object_unref(queue_video_sink);
        gst_object_unref(tee_video_pad);
        gst_bin_remove_many(GST_BIN(pipeline), webrtc, video_queue, audio_queue, NULL);
        return NULL;
    }
    gst_object_unref(queue_video_sink);

    GstPad *queue_video_src = gst_element_get_static_pad(video_queue, "src");
    GstPad *webrtc_video_sink = gst_element_get_request_pad(webrtc, "sink_%u");
    if (gst_pad_link(queue_video_src, webrtc_video_sink) != GST_PAD_LINK_OK) {
        g_printerr("[Server] Failed to link video queue to webrtc\n");
        gst_object_unref(queue_video_src);
        gst_object_unref(webrtc_video_sink);
        gst_element_release_request_pad(video_tee, tee_video_pad);
        gst_object_unref(tee_video_pad);
        gst_bin_remove_many(GST_BIN(pipeline), webrtc, video_queue, audio_queue, NULL);
        return NULL;
    }
    gst_object_unref(queue_video_src);
    gst_object_unref(webrtc_video_sink);

    GstPad *tee_audio_pad = gst_element_get_request_pad(audio_tee, "src_%u");
    GstPad *queue_audio_sink = gst_element_get_static_pad(audio_queue, "sink");
    if (gst_pad_link(tee_audio_pad, queue_audio_sink) != GST_PAD_LINK_OK) {
        g_printerr("[Server] Failed to link audio tee to queue\n");
        gst_object_unref(queue_audio_sink);
        gst_element_release_request_pad(video_tee, tee_video_pad);
        gst_object_unref(tee_video_pad);
        gst_object_unref(tee_audio_pad);
        gst_bin_remove_many(GST_BIN(pipeline), webrtc, video_queue, audio_queue, NULL);
        return NULL;
    }
    gst_object_unref(queue_audio_sink);

    GstPad *queue_audio_src = gst_element_get_static_pad(audio_queue, "src");
    GstPad *webrtc_audio_sink = gst_element_get_request_pad(webrtc, "sink_%u");
    if (gst_pad_link(queue_audio_src, webrtc_audio_sink) != GST_PAD_LINK_OK) {
        g_printerr("[Server] Failed to link audio queue to webrtc\n");
        gst_object_unref(queue_audio_src);
        gst_object_unref(webrtc_audio_sink);
        gst_element_release_request_pad(video_tee, tee_video_pad);
        gst_object_unref(tee_video_pad);
        gst_element_release_request_pad(audio_tee, tee_audio_pad);
        gst_object_unref(tee_audio_pad);
        gst_bin_remove_many(GST_BIN(pipeline), webrtc, video_queue, audio_queue, NULL);
        return NULL;
    }
    gst_object_unref(queue_audio_src);
    gst_object_unref(webrtc_audio_sink);

    std::lock_guard<std::mutex> lock(peers_mutex);
    auto& peer = peers[peer_id];
    peer.video_tee_pad = tee_video_pad;
    peer.audio_tee_pad = tee_audio_pad;
    peer.video_queue = video_queue;
    peer.audio_queue = audio_queue;
    peer.webrtc = webrtc;

    gchar *peer_id_copy1 = g_strdup(peer_id.c_str());
    gchar *peer_id_copy2 = g_strdup(peer_id.c_str());
    gchar *peer_id_copy3 = g_strdup(peer_id.c_str());
    gchar *peer_id_copy4 = g_strdup(peer_id.c_str());
    
    peer.negotiation_handler = g_signal_connect(webrtc, "on-negotiation-needed", 
                                                  G_CALLBACK(on_negotiation_needed), peer_id_copy1);
    peer.ice_candidate_handler = g_signal_connect(webrtc, "on-ice-candidate", 
                                                    G_CALLBACK(on_ice_candidate), peer_id_copy2);
    peer.ice_gathering_handler = g_signal_connect(webrtc, "notify::ice-gathering-state", 
                                                    G_CALLBACK(on_ice_gathering_state_notify), peer_id_copy3);
    peer.ice_connection_handler = g_signal_connect(webrtc, "notify::ice-connection-state", 
                                                     G_CALLBACK(on_ice_connection_state_notify), peer_id_copy4);

    gst_element_sync_state_with_parent(video_queue);
    gst_element_sync_state_with_parent(audio_queue);
    gst_element_sync_state_with_parent(webrtc);

    g_print("[Server] ✓ Added WebRTC peer: %s (%s mode)\n", peer_id.c_str(), 
            use_internet_mode ? "Internet" : "LAN");
    
    return webrtc;
}

static gboolean remove_peer_async(gpointer user_data) {
    std::string* peer_id_ptr = static_cast<std::string*>(user_data);
    std::string peer_id = *peer_id_ptr;
    delete peer_id_ptr;

    std::lock_guard<std::mutex> lock(peers_mutex);
    auto it = peers.find(peer_id);
    if (it == peers.end()) return G_SOURCE_REMOVE;

    PeerState& peer = it->second;
    if (peer.is_cleaning_up) return G_SOURCE_REMOVE;
    peer.is_cleaning_up = TRUE;

    g_print("[Server] Cleaning up peer: %s\n", peer_id.c_str());

    if (peer.webrtc) {
        if (peer.negotiation_handler) {
            g_signal_handler_disconnect(peer.webrtc, peer.negotiation_handler);
            peer.negotiation_handler = 0;
        }
        if (peer.ice_candidate_handler) {
            g_signal_handler_disconnect(peer.webrtc, peer.ice_candidate_handler);
            peer.ice_candidate_handler = 0;
        }
        if (peer.ice_gathering_handler) {
            g_signal_handler_disconnect(peer.webrtc, peer.ice_gathering_handler);
            peer.ice_gathering_handler = 0;
        }
        if (peer.ice_connection_handler) {
            g_signal_handler_disconnect(peer.webrtc, peer.ice_connection_handler);
            peer.ice_connection_handler = 0;
        }

        gst_element_set_locked_state(peer.webrtc, TRUE);
        if (peer.video_queue) gst_element_set_locked_state(peer.video_queue, TRUE);
        if (peer.audio_queue) gst_element_set_locked_state(peer.audio_queue, TRUE);

        gst_element_set_state(peer.webrtc, GST_STATE_NULL);
        if (peer.video_queue) gst_element_set_state(peer.video_queue, GST_STATE_NULL);
        if (peer.audio_queue) gst_element_set_state(peer.audio_queue, GST_STATE_NULL);

        if (peer.video_queue) {
            GstPad *sink_pad = gst_element_get_static_pad(peer.video_queue, "sink");
            if (sink_pad) {
                gst_pad_send_event(sink_pad, gst_event_new_flush_start());
                gst_pad_send_event(sink_pad, gst_event_new_flush_stop(FALSE));
                gst_object_unref(sink_pad);
            }
        }
        if (peer.audio_queue) {
            GstPad *sink_pad = gst_element_get_static_pad(peer.audio_queue, "sink");
            if (sink_pad) {
                gst_pad_send_event(sink_pad, gst_event_new_flush_start());
                gst_pad_send_event(sink_pad, gst_event_new_flush_stop(FALSE));
                gst_object_unref(sink_pad);
            }
        }

        if (video_tee && peer.video_tee_pad) {
            gst_element_release_request_pad(video_tee, peer.video_tee_pad);
            gst_object_unref(peer.video_tee_pad);
            peer.video_tee_pad = NULL;
        }
        if (audio_tee && peer.audio_tee_pad) {
            gst_element_release_request_pad(audio_tee, peer.audio_tee_pad);
            gst_object_unref(peer.audio_tee_pad);
            peer.audio_tee_pad = NULL;
        }

        if (peer.video_queue && peer.audio_queue) {
            gst_bin_remove_many(GST_BIN(pipeline), peer.webrtc, peer.video_queue, peer.audio_queue, NULL);
        } else if (peer.webrtc) {
            gst_bin_remove(GST_BIN(pipeline), peer.webrtc);
        }

        peer.webrtc = NULL;
        peer.video_queue = NULL;
        peer.audio_queue = NULL;
    }

    while (!peer.pending_ice_candidates.empty()) {
        peer.pending_ice_candidates.pop();
    }

    peers.erase(it);
    g_print("[Server] ✓ Removed peer: %s (Active peers: %zu)\n", peer_id.c_str(), peers.size());

    return G_SOURCE_REMOVE;
}

static void remove_webrtc_peer(const std::string& peer_id) {
    std::string* peer_id_copy = new std::string(peer_id);
    g_idle_add(remove_peer_async, peer_id_copy);
}

static void flush_pending_ice_candidates(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(peers_mutex);
    auto it = peers.find(peer_id);
    if (it == peers.end() || !it->second.webrtc || !it->second.remote_description_set) return;
    
    PeerState &peer = it->second;
    if (peer.pending_ice_candidates.empty()) return;
    
    g_print("[Server] Flushing %zu pending ICE candidates for %s\n", 
            peer.pending_ice_candidates.size(), peer_id.c_str());
    
    while (!peer.pending_ice_candidates.empty()) {
        IceCandidate ice = peer.pending_ice_candidates.front();
        peer.pending_ice_candidates.pop();
        g_signal_emit_by_name(peer.webrtc, "add-ice-candidate", ice.mlineindex, ice.candidate.c_str());
    }
}

static void send_ice_candidate_to_peer(const std::string& peer_id, guint mlineindex, const gchar *candidate) {
    JsonObject *ice = json_object_new();
    json_object_set_string_member(ice, "candidate", candidate);
    json_object_set_int_member(ice, "sdpMLineIndex", mlineindex);

    JsonObject *msg = json_object_new();
    json_object_set_string_member(msg, "type", "ice-candidate");
    json_object_set_string_member(msg, "from", sender_id);
    json_object_set_object_member(msg, "candidate", ice);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, msg);
    gchar *text = json_to_string(node, FALSE);
    
    send_to_client(peer_id, text);
    
    g_free(text);
    json_node_free(node);
    json_object_unref(msg);
}

static void on_ice_candidate(GstElement *webrtc, guint mlineindex, gchar *candidate, gpointer user_data) {
    gchar *peer_id = (gchar*)user_data;
    if (!peer_id) return;
    
    std::string peer_id_str(peer_id);
    
    std::lock_guard<std::mutex> lock(peers_mutex);
    auto it = peers.find(peer_id_str);
    if (it == peers.end() || it->second.is_cleaning_up) return;
    
    gboolean is_host = strstr(candidate, "typ host") != NULL;
    gboolean is_srflx = strstr(candidate, "typ srflx") != NULL;
    gboolean is_relay = strstr(candidate, "typ relay") != NULL;
    gboolean is_private = is_rfc1918_ip(candidate);
    
    if (it->second.use_internet_mode) {
        const char* type = is_host ? "host" : is_srflx ? "srflx" : is_relay ? "relay" : "unknown";
        g_print("[Server] → Sending %s candidate to %s\n", type, peer_id);
        send_ice_candidate_to_peer(peer_id_str, mlineindex, candidate);
    } else {
        if (is_host && is_private) {
            g_print("[Server] ✓ Sending LAN host candidate to %s\n", peer_id);
            send_ice_candidate_to_peer(peer_id_str, mlineindex, candidate);
        } else {
            g_print("[Server] 🚫 Filtered (%s %s) for %s\n", 
                    is_host ? "host" : is_srflx ? "srflx" : is_relay ? "relay" : "unknown",
                    is_private ? "private" : "public",
                    peer_id);
        }
    }
}

static void force_create_offer(const std::string& peer_id);

static void on_offer_created(GstPromise *promise, gpointer user_data) {
    gchar *peer_id = (gchar*)user_data;
    if (!peer_id) {
        gst_promise_unref(promise);
        return;
    }
    
    std::string peer_id_str(peer_id);
    
    {
        std::lock_guard<std::mutex> lock(peers_mutex);
        auto it = peers.find(peer_id_str);
        if (it == peers.end() || it->second.is_cleaning_up) {
            g_free(peer_id);
            gst_promise_unref(promise);
            return;
        }
    }
    
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    if (!offer) {
        g_printerr("[Server] Failed to create offer for %s\n", peer_id);
        std::lock_guard<std::mutex> lock(peers_mutex);
        auto it = peers.find(peer_id_str);
        if (it != peers.end()) {
            it->second.offer_in_progress = FALSE;
        }
        g_free(peer_id);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(peers_mutex);
        auto it = peers.find(peer_id_str);
        if (it == peers.end() || it->second.is_cleaning_up || !it->second.webrtc) {
            gst_webrtc_session_description_free(offer);
            g_free(peer_id);
            return;
        }

        GstPromise *local_promise = gst_promise_new();
        g_signal_emit_by_name(it->second.webrtc, "set-local-description", offer, local_promise);
        gst_promise_interrupt(local_promise);
        gst_promise_unref(local_promise);
    }

    gchar *sdp_text = gst_sdp_message_as_text(offer->sdp);
    g_print("[Server] ✓ Offer created for %s\n", peer_id);
    
    JsonObject *msg = json_object_new();
    json_object_set_string_member(msg, "type", "offer");
    json_object_set_string_member(msg, "from", sender_id);
    json_object_set_string_member(msg, "sdp", sdp_text);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, msg);
    gchar *text = json_to_string(node, FALSE);
    
    send_to_client(peer_id_str, text);
    g_free(text);
    json_node_free(node);
    json_object_unref(msg);
    
    g_free(sdp_text);
    gst_webrtc_session_description_free(offer);
    g_free(peer_id);
}

static void force_create_offer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(peers_mutex);
    auto it = peers.find(peer_id);
    if (it == peers.end() || !it->second.webrtc || it->second.is_cleaning_up) {
        g_printerr("[Server] Cannot create offer - peer %s not found\n", peer_id.c_str());
        return;
    }
    if (it->second.offer_in_progress) {
        g_print("[Server] Offer already in progress for %s\n", peer_id.c_str());
        return;
    }
    g_print("[Server] Creating offer for %s...\n", peer_id.c_str());
    it->second.offer_in_progress = TRUE;
    
    gchar *peer_id_copy = g_strdup(peer_id.c_str());
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, peer_id_copy, NULL);
    g_signal_emit_by_name(it->second.webrtc, "create-offer", NULL, promise);
}

static void on_negotiation_needed(GstElement *element, gpointer user_data) {
    gchar *peer_id = (gchar*)user_data;
    g_print("[Server] Negotiation needed for %s\n", peer_id ? peer_id : "unknown");
}

static void on_ice_gathering_state_notify(GstElement *webrtc, GParamSpec *pspec, gpointer user_data) {
    gchar *peer_id = (gchar*)user_data;
    GstWebRTCICEGatheringState state;
    g_object_get(webrtc, "ice-gathering-state", &state, NULL);
    const gchar *state_str = (state == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) ? "complete" : "gathering";
    g_print("[Server] ICE gathering %s for %s\n", state_str, peer_id ? peer_id : "unknown");
}

static void on_ice_connection_state_notify(GstElement *webrtc, GParamSpec *pspec, gpointer user_data) {
    gchar *peer_id = (gchar*)user_data;
    if (!peer_id) return;
    
    std::string peer_id_str(peer_id);
    
    std::lock_guard<std::mutex> lock(peers_mutex);
    auto it = peers.find(peer_id_str);
    if (it == peers.end() || it->second.is_cleaning_up) return;
    
    GstWebRTCICEConnectionState state;
    g_object_get(webrtc, "ice-connection-state", &state, NULL);
    if (state == GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED) {
        g_print("[Server] ✓✓✓ ICE connected for %s (%s mode) ✓✓✓\n", 
                peer_id, it->second.use_internet_mode ? "Internet" : "LAN");
    } else if (state == GST_WEBRTC_ICE_CONNECTION_STATE_FAILED) {
        g_printerr("[Server] ✗ ICE connection failed for %s\n", peer_id);
    }
}

static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data) {
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;
            gst_message_parse_error(message, &err, &debug);
            g_printerr("[Server] ✗ Pipeline Error: %s\n", err->message);
            if (debug) g_printerr("[Server] Debug: %s\n", debug);
            g_error_free(err);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err;
            gchar *debug;
            gst_message_parse_warning(message, &err, &debug);
            g_printerr("[Server] ⚠ Pipeline Warning: %s\n", err->message);
            g_error_free(err);
            g_free(debug);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

// ==================== Message Handling ====================

static void handle_viewer_message(const std::string& from_id, JsonObject* object) {
    const gchar *msg_type = json_object_get_string_member(object, "type");

    if (g_strcmp0(msg_type, "request-offer") == 0) {
        gboolean use_internet = FALSE;
        if (json_object_has_member(object, "internetMode")) {
            use_internet = json_object_get_boolean_member(object, "internetMode");
        }
        
        g_print("[Server] ✓ request-offer from %s (mode: %s)\n", 
                from_id.c_str(), use_internet ? "Internet" : "LAN");
        
        if (!pipeline) {
            if (!build_base_pipeline()) {
                g_printerr("[Server] Failed to build base pipeline\n");
                return;
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(peers_mutex);
            if (peers.find(from_id) != peers.end()) {
                g_print("[Server] Peer %s reconnecting, removing old connection\n", from_id.c_str());
                remove_webrtc_peer(from_id);
                g_usleep(300000);
            }
        }
        
        GstElement *webrtc = add_webrtc_peer(from_id, use_internet);
        if (!webrtc) {
            g_printerr("[Server] Failed to add peer %s\n", from_id.c_str());
            return;
        }
        
        {
            std::lock_guard<std::mutex> lock(peers_mutex);
            peers[from_id].peer_id = from_id;
            peers[from_id].use_internet_mode = use_internet;
            peers[from_id].offer_in_progress = FALSE;
            peers[from_id].remote_description_set = FALSE;
            peers[from_id].is_cleaning_up = FALSE;
            
            g_print("[Server] Active peers: %zu\n", peers.size());
        }
        
        g_usleep(200000);
        force_create_offer(from_id);
        
    } else if (g_strcmp0(msg_type, "answer") == 0) {
        const gchar *sdp_text = json_object_get_string_member(object, "sdp");
        g_print("[Server] ✓ answer from %s\n", from_id.c_str());

        GstElement *webrtc = NULL;
        {
            std::lock_guard<std::mutex> lock(peers_mutex);
            auto it = peers.find(from_id);
            if (it == peers.end() || !it->second.webrtc || it->second.is_cleaning_up) {
                g_printerr("[Server] Peer %s not found for answer\n", from_id.c_str());
                return;
            }
            webrtc = it->second.webrtc;
        }

        GstSDPMessage *sdp;
        gst_sdp_message_new(&sdp);
        if (gst_sdp_message_parse_buffer((guint8 *)sdp_text, strlen(sdp_text), sdp) != GST_SDP_OK) {
            g_printerr("[Server] Failed to parse SDP answer from %s\n", from_id.c_str());
            gst_sdp_message_free(sdp);
            return;
        }

        GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        GstPromise *promise = gst_promise_new();
        g_signal_emit_by_name(webrtc, "set-remote-description", answer, promise);
        gst_promise_interrupt(promise);
        gst_promise_unref(promise);
        gst_webrtc_session_description_free(answer);
        
        {
            std::lock_guard<std::mutex> lock(peers_mutex);
            auto it = peers.find(from_id);
            if (it != peers.end() && !it->second.is_cleaning_up) {
                it->second.remote_description_set = TRUE;
                it->second.offer_in_progress = FALSE;
            }
        }
        
        flush_pending_ice_candidates(from_id);
        
    } else if (g_strcmp0(msg_type, "ice-candidate") == 0) {
        if (!json_object_has_member(object, "candidate")) return;
        JsonObject *candidate_obj = json_object_get_object_member(object, "candidate");
        if (!candidate_obj) return;
        
        const gchar *candidate_str = json_object_get_string_member(candidate_obj, "candidate");
        if (!candidate_str || strlen(candidate_str) == 0) return;
        
        guint sdp_mline_index = json_object_get_int_member(candidate_obj, "sdpMLineIndex");
        
        g_print("[Server] Received ICE [%u] from %s\n", sdp_mline_index, from_id.c_str());
        
        std::lock_guard<std::mutex> lock(peers_mutex);
        auto it = peers.find(from_id);
        if (it == peers.end() || !it->second.webrtc || it->second.is_cleaning_up) {
            g_printerr("[Server] Peer %s not found for ICE candidate\n", from_id.c_str());
            return;
        }
        
        if (!it->second.remote_description_set) {
            IceCandidate ice;
            ice.mlineindex = sdp_mline_index;
            ice.candidate = candidate_str;
            it->second.pending_ice_candidates.push(ice);
            g_print("[Server] Queued ICE candidate for %s (waiting for remote description)\n", from_id.c_str());
            return;
        }
        
        g_signal_emit_by_name(it->second.webrtc, "add-ice-candidate", sdp_mline_index, candidate_str);
    }
}

// ==================== WebSocket Handler ====================

static void on_ws_message(SoupWebsocketConnection* conn, SoupWebsocketDataType type,
                          GBytes* message, gpointer user_data) {
    if (type != SOUP_WEBSOCKET_DATA_TEXT) return;
    std::string* client_id = static_cast<std::string*>(user_data);

    gsize size = 0;
    const gchar* data = static_cast<const gchar*>(g_bytes_get_data(message, &size));
    gchar* text = g_strndup(data, size);

    JsonParser* parser = json_parser_new();
    if (!json_parser_load_from_data(parser, text, -1, NULL)) { 
        g_free(text); 
        g_object_unref(parser); 
        return; 
    }

    JsonNode* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) { 
        g_free(text); 
        g_object_unref(parser); 
        return; 
    }
    JsonObject* object = json_node_get_object(root);

    if (!json_object_has_member(object, "type")) { 
        g_free(text); 
        g_object_unref(parser); 
        return; 
    }

    handle_viewer_message(*client_id, object);
    g_free(text);
    g_object_unref(parser);
}

static void on_ws_closed(SoupWebsocketConnection* conn, gpointer user_data) {
    std::string* client_id = static_cast<std::string*>(user_data);
    g_print("[Server] Client disconnected: %s\n", client_id->c_str());

    remove_webrtc_peer(*client_id);
    remote_clients.erase(*client_id);
    delete client_id;
}

static void on_websocket_handler(SoupServer* server, SoupWebsocketConnection* conn,
                                 const char* path, SoupClientContext* client, gpointer user_data)
{
    (void)server; (void)path; (void)client; (void)user_data;
    std::string client_id = make_id();
    std::string* id_ptr = new std::string(client_id);

    remote_clients[client_id] = conn;
    g_object_ref(conn);

    JsonObject* reg_msg = json_object_new();
    json_object_set_string_member(reg_msg, "type", "registered");
    json_object_set_string_member(reg_msg, "id", client_id.c_str());
    JsonNode* node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, reg_msg);
    gchar* text = json_to_string(node, FALSE);
    soup_websocket_connection_send_text(conn, text);
    g_free(text); 
    json_node_free(node); 
    json_object_unref(reg_msg);

    g_signal_connect(conn, "message", G_CALLBACK(on_ws_message), id_ptr);
    g_signal_connect(conn, "closed",  G_CALLBACK(on_ws_closed),  id_ptr);
    
    g_print("[Server] ✓ New client connected: %s (Total: %zu)\n", client_id.c_str(), remote_clients.size());
}

// ==================== Main ====================

static void print_usage(const char *prog_name) {
    g_print("Usage: %s [OPTIONS]\n", prog_name);
    g_print("\nMulti-Client Adaptive WebRTC Server - Supports both LAN and Internet streaming\n");
    g_print("\nOptions:\n");
    g_print("  --codec=CODEC       h264 or h265 (default: h264)\n");
    g_print("  --bitrate=KBPS      Video bitrate (default: 2000)\n");
    g_print("  --fps=FPS           Framerate (default: 30)\n");
    g_print("  --width=WIDTH       Width (default: 1280)\n");
    g_print("  --height=HEIGHT     Height (default: 720)\n");
    g_print("  --device=PATH       Camera (default: /dev/video0)\n");
    g_print("  --adev=ALSA         Audio device (default: hw:1,1)\n");
    g_print("  --port=PORT         Server port (default: 8080)\n");
    g_print("  --www=PATH          Static files directory (default: public)\n");
    g_print("  --help              Show this help\n");
    g_print("\nNote: Supports unlimited simultaneous viewers!\n");
}

static gboolean parse_arguments(int argc, char *argv[]) {
    config.codec = g_strdup("h264");
    config.bitrate = 2000;
    config.fps = 30;
    config.width = 1280;
    config.height = 720;
    config.device = g_strdup("/dev/video0");
    config.adev = g_strdup("hw:1,1");
    config.port = 8080;
    config.www_root = g_strdup("public");

    struct option long_options[] = {
        {"codec",       required_argument, 0, 'c'},
        {"bitrate",     required_argument, 0, 'b'},
        {"fps",         required_argument, 0, 'f'},
        {"width",       required_argument, 0, 'w'},
        {"height",      required_argument, 0, 'H'},
        {"device",      required_argument, 0, 'd'},
        {"adev",        required_argument, 0, 'a'},
        {"port",        required_argument, 0, 'p'},
        {"www",         required_argument, 0, 'W'},
        {"help",        no_argument,       0, '?'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;

    while ((c = getopt_long(argc, argv, "c:b:f:w:H:d:a:p:W:?", long_options, &option_index)) != -1) {
        switch (c) {
            case 'c':
                g_free(config.codec);
                config.codec = g_strdup(optarg);
                break;
            case 'b':
                config.bitrate = atoi(optarg);
                break;
            case 'f':
                config.fps = atoi(optarg);
                break;
            case 'w':
                config.width = atoi(optarg);
                break;
            case 'H':
                config.height = atoi(optarg);
                break;
            case 'd':
                g_free(config.device);
                config.device = g_strdup(optarg);
                break;
            case 'a':
                g_free(config.adev);
                config.adev = g_strdup(optarg);
                break;
            case 'p':
                config.port = atoi(optarg);
                break;
            case 'W':
                g_free(config.www_root);
                config.www_root = g_strdup(optarg);
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
    srand((unsigned)time(NULL));
    gst_init(&argc, &argv);

    if (!parse_arguments(argc, argv)) {
        return -1;
    }

    g_print("\n");
    g_print("╔═══════════════════════════════════════════════════╗\n");
    g_print("║   Multi-Client WebRTC Streaming Server           ║\n");
    g_print("║   Supports: LAN (direct) + Internet (TURN/STUN)  ║\n");
    g_print("║   🎥 Multiple simultaneous viewers supported      ║\n");
    g_print("║   ✨ Enhanced stability for rapid reconnects     ║\n");
    g_print("╚═══════════════════════════════════════════════════╝\n");
    g_print("\n");
    g_print("┌─── Configuration ───\n");
    g_print("  Codec:      %s\n", config.codec);
    g_print("  Resolution: %dx%d @ %d fps\n", config.width, config.height, config.fps);
    g_print("  Bitrate:    %d kbps\n", config.bitrate);
    g_print("  Device:     %s\n", config.device);
    g_print("  Audio:      %s\n", config.adev);
    g_print("  Port:       %u\n", config.port);
    g_print("  WWW Root:   %s\n", config.www_root);
    g_print("\n");
    g_print("┌─── Network Support ───\n");
    g_print("  🏠 LAN Mode:      Direct connection (no STUN/TURN)\n");
    g_print("  🌍 Internet Mode: Full TURN/STUN relay support\n");
    g_print("  📱 Client selects mode automatically or manually\n");
    g_print("  👥 Unlimited simultaneous viewers\n");
    g_print("  🔄 Robust reconnection handling\n");
    g_print("\n");
    g_print("Press Ctrl+C to stop\n");
    g_print("─────────────────────────────────────────────────────\n\n");

    sender_id = g_strdup(make_id().c_str());
    loop = g_main_loop_new(NULL, FALSE);

    http_server = soup_server_new(NULL, NULL);
    GError* error = NULL;
    
    if (!soup_server_listen_all(http_server, config.port, (SoupServerListenOptions)0, &error)) {
        g_printerr("[Server] Failed to start: %s\n", error->message);
        g_error_free(error);
        g_object_unref(http_server);
        g_main_loop_unref(loop);
        g_free(config.codec);
        g_free(config.device);
        g_free(config.adev);
        g_free(config.www_root);
        g_free(sender_id);
        return 1;
    }

    soup_server_add_handler(http_server, "/", static_handler, NULL, NULL);
    soup_server_add_websocket_handler(http_server, "/ws", NULL, NULL,
                                      on_websocket_handler, NULL, NULL);

    g_print("[Server] ✓✓✓ Ready at http://localhost:%u/ ✓✓✓\n\n", config.port);

    g_main_loop_run(loop);

    g_print("\n[Main] Cleaning up...\n");
    
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (video_tee) gst_object_unref(video_tee);
        if (audio_tee) gst_object_unref(audio_tee);
        gst_object_unref(pipeline);
    }
    
    peers.clear();
    
    for (auto& pair : remote_clients) {
        g_object_unref(pair.second);
    }
    remote_clients.clear();
    
    g_object_unref(http_server);
    g_main_loop_unref(loop);
    g_free(sender_id);
    g_free(config.codec);
    g_free(config.device);
    g_free(config.adev);
    g_free(config.www_root);

    return 0;
}
