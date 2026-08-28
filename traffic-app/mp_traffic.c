#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include "picoquic.h"
#include "picoquic_utils.h"
#include "picoquic_packet_loop.h"
#include "picoquic_internal.h" /* opaque picoquic_cnx_t in picoquic.h hides
    the fields (nb_paths, is_multipath_enabled, ...) this file needs to
    read directly - picoquicdemo.c includes this same internal header for
    the identical reason, since it's part of the same source tree rather
    than a true external API consumer. */
#include "picoquic_qlog.h"
#include "pacer.h"

#define MP_VIDEO_CHUNK_BYTES 1200
#define MP_VIDEO_TARGET_BPS 10000000ULL

#define MP_MAX_ALT_PATHS 8 /* matches picoquic's own PICOQUIC_NB_PATH_TARGET
    default (picoquic_internal.h) - hardcoded rather than referencing that
    macro directly, since this array-sizing constant doesn't need to track
    picoquic's internal default if it ever changes; 8 is generous headroom
    for this rig's 3 links either way. */

typedef struct st_mp_config_t {
    int is_server;
    int port;
    char server_ip[64];
    char path_spec[256];   /* raw -A value, parsed further in Task 3 */
    char cc_algo[32];
    int duration_sec;
    char qlog_dir[256];    /* empty = no qlog, matching picoquicdemo's -q */
} mp_config_t;

static void mp_usage(const char* prog) {
    fprintf(stderr, "Server: %s -p <port> [-G <cc_algo>] [-q <qlog_dir>]\n", prog);
    fprintf(stderr, "Client: %s -A <path_spec> [-G <cc_algo>] [-q <qlog_dir>] --duration <seconds> <server_ip> <port>\n", prog);
}

/* Returns 0 on success, -1 on bad arguments (mp_usage already printed). */
int mp_parse_args(int argc, char** argv, mp_config_t* config) {
    memset(config, 0, sizeof(mp_config_t));
    strncpy(config->cc_algo, "cubic", sizeof(config->cc_algo) - 1);
    config->duration_sec = 30;

    int opt;
    /* getopt_long needed for --duration; picoquic's own getopt.c (used by
       picoquicdemo) is short-option-only per Phase 0 research, so this
       app uses the system getopt_long instead. */
    static struct option long_opts[] = {
        {"duration", required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "p:A:G:q:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p':
            config->port = atoi(optarg);
            config->is_server = 1;
            break;
        case 'A':
            strncpy(config->path_spec, optarg, sizeof(config->path_spec) - 1);
            break;
        case 'G':
            strncpy(config->cc_algo, optarg, sizeof(config->cc_algo) - 1);
            break;
        case 'q':
            strncpy(config->qlog_dir, optarg, sizeof(config->qlog_dir) - 1);
            break;
        case 'd':
            config->duration_sec = atoi(optarg);
            break;
        default:
            mp_usage(argv[0]);
            return -1;
        }
    }

    if (!config->is_server) {
        /* Client mode: expect <server_ip> <port> as positional args. */
        if (optind + 2 > argc) {
            fprintf(stderr, "Client mode requires server_ip and port.\n");
            mp_usage(argv[0]);
            return -1;
        }
        strncpy(config->server_ip, argv[optind], sizeof(config->server_ip) - 1);
        config->port = atoi(argv[optind + 1]);
    }

    return 0;
}

/* --- -A path-spec parser, copied from picoquicdemo.c's
   picoquic_parse_client_multipath_config() - that function is defined
   locally in picoquicdemo.c, not exported by the picoquic library, so it
   can't be linked against and must be copied to guarantee identical
   parsing behavior to what's already validated all session. */
static char* mp_strsep(char** stringp, const char* delim) {
    return strsep(stringp, delim);
}

static int mp_parse_client_multipath_config(char* mp_config, int* src_if,
    struct sockaddr_storage* alt_client_ip, struct sockaddr_storage* alt_server_ip,
    int* nb_alt_paths, struct sockaddr_storage* default_server_ip) {
    int ret = 0;
    size_t config_len = strlen(mp_config) + 1;
    int valid_new_entry = 0;
    char *token, *token2, *end_ptr, *alt_path, *ptr, *str;
    uint16_t server_port = (default_server_ip->ss_family == AF_INET) ?
        ((struct sockaddr_in*)default_server_ip)->sin_port :
        ((struct sockaddr_in6*)default_server_ip)->sin6_port;
    str = malloc(sizeof(char) * config_len);
    alt_path = malloc(sizeof(char) * config_len);
    if (str == NULL || alt_path == NULL) {
        return -1;
    }
    memcpy(str, mp_config, sizeof(char) * config_len);
    ptr = str;

    while ((token = mp_strsep(&str, ","))) {
        struct sockaddr_storage ip;
        valid_new_entry = 0;
        memcpy(alt_path, token, sizeof(char) * (strnlen(token, config_len - 1) + 1));

        if ((token2 = mp_strsep(&token, "/")) != 0) {
            if (picoquic_store_text_addr(&ip, token2, 0) == 0) {
                memcpy(alt_client_ip + (*nb_alt_paths), &ip, sizeof(struct sockaddr_storage));
                if ((token2 = mp_strsep(&token, "/")) == NULL) {
                    *(src_if + (*nb_alt_paths)) = 0;
                    memcpy(alt_server_ip + (*nb_alt_paths), default_server_ip, sizeof(struct sockaddr_storage));
                    valid_new_entry = 1;
                } else {
                    *(src_if + (*nb_alt_paths)) = (int)strtol(token2, &end_ptr, 10);
                    if (*end_ptr) {
                        fprintf(stderr, "mp_traffic: unexpected interface index %s, skipping path %s\n", token2, alt_path);
                    } else if (token) {
                        if (picoquic_store_text_addr(&ip, token, server_port) == 0) {
                            memcpy(alt_server_ip + (*nb_alt_paths), &ip, sizeof(struct sockaddr_storage));
                            valid_new_entry = 1;
                        }
                    } else {
                        memcpy(alt_server_ip + (*nb_alt_paths), default_server_ip, sizeof(struct sockaddr_storage));
                        valid_new_entry = 1;
                    }
                }
            }
        }

        if (valid_new_entry == 1 && alt_client_ip[*nb_alt_paths].ss_family == alt_server_ip[*nb_alt_paths].ss_family) {
            (*nb_alt_paths)++;
            if (*nb_alt_paths >= PICOQUIC_NB_PATH_TARGET) {
                break;
            }
        }
    }
    free(alt_path);
    free(ptr);
    return ret;
}

/* --- Client connection setup and multipath driving. Adapted from
   picoquicdemo.c's client_loop_cb_t / client_loop_cb() /
   client_create_additional_path(), trimmed of the migration/key-update/
   quicperf branches this app doesn't need. Two separate callbacks are
   involved, easy to conflate: mp_client_callback (below) handles
   per-CONNECTION events (ready/close, and traffic generation in later
   tasks) via picoquic_set_callback; mp_client_loop_cb handles the
   packet-loop's own socket-level events and is where multipath path
   creation is actually driven from - picoquicdemo.c does the same split. */
typedef struct st_mp_client_loop_cb_t {
    picoquic_cnx_t* cnx_client;
    int multipath_initiated;
    int multipath_probe_done;
    struct sockaddr_storage server_address;
    struct sockaddr_storage client_alt_address[MP_MAX_ALT_PATHS];
    struct sockaddr_storage server_alt_address[MP_MAX_ALT_PATHS];
    int client_alt_if[MP_MAX_ALT_PATHS];
    int client_alt_state[MP_MAX_ALT_PATHS];
    int nb_alt_paths;
    uint16_t alt_port;
} mp_client_loop_cb_t;

static uint64_t g_connection_ready_time = 0;

/* Video-like traffic generator: client -> server, via QUIC datagrams
   (RFC 9221), constant-bitrate. Datagrams are chosen deliberately over a
   stream here - loss-tolerant, no head-of-line blocking holding up stale
   data, the standard choice for real-time-media-like traffic. Uses
   picoquic_queue_datagram_frame, a direct push API (confirmed against
   picoquic's own picohttp/quicperf.c, which already implements proven
   CBR pacing this way) rather than the pull-based prepare_datagram
   callback - simpler, and avoids inventing a new pacing pattern when a
   working one already exists in picoquic's own codebase. */
static mp_pacer_t g_video_pacer;
static int g_video_pacer_started = 0;

static void mp_client_send_video_if_due(picoquic_cnx_t* cnx) {
    uint64_t now = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));

    if (!g_video_pacer_started) {
        mp_pacer_init(&g_video_pacer, now, MP_VIDEO_CHUNK_BYTES, MP_VIDEO_TARGET_BPS);
        g_video_pacer_started = 1;
    }

    while (mp_pacer_is_due(&g_video_pacer, now)) {
        uint8_t chunk[MP_VIDEO_CHUNK_BYTES];
        memset(chunk, 0xAA, sizeof(chunk)); /* filler payload - content doesn't matter, see spec */
        picoquic_queue_datagram_frame(cnx, sizeof(chunk), chunk);
        mp_pacer_advance(&g_video_pacer);
    }

    picoquic_set_app_wake_time(cnx, mp_pacer_next_time(&g_video_pacer));
}

static int mp_client_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes,
    size_t length, picoquic_call_back_event_t event, void* callback_ctx, void* v_stream_ctx) {
    (void)stream_id; (void)bytes; (void)length; (void)callback_ctx; (void)v_stream_ctx;

    switch (event) {
    case picoquic_callback_ready:
        g_connection_ready_time = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));
        fprintf(stderr, "mp_traffic: connection ready at %llu\n",
            (unsigned long long)g_connection_ready_time);
        mp_client_send_video_if_due(cnx);
        break;
    case picoquic_callback_app_wakeup:
        mp_client_send_video_if_due(cnx);
        break;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
        fprintf(stderr, "mp_traffic: connection closed\n");
        break;
    default:
        break;
    }
    return 0;
}

static int mp_client_create_additional_path(picoquic_cnx_t* cnx, mp_client_loop_cb_t* cb_ctx) {
    int ret = 0;
    int need_to_wait = 0;

    for (int i = 0; i < cb_ctx->nb_alt_paths; i++) {
        if (cb_ctx->client_alt_state[i] != 0) {
            continue;
        }

        cb_ctx->client_alt_state[i] = 1;
        ret = picoquic_probe_new_path_ex(cb_ctx->cnx_client,
            (struct sockaddr*)&cb_ctx->server_alt_address[i],
            (struct sockaddr*)&cb_ctx->client_alt_address[i],
            cb_ctx->client_alt_if[i],
            picoquic_get_quic_time(picoquic_get_quic_ctx(cnx)), 0);

        if (ret != 0) {
            if (ret == PICOQUIC_ERROR_PATH_ID_BLOCKED ||
                ret == PICOQUIC_ERROR_PATH_CID_BLOCKED ||
                ret == PICOQUIC_ERROR_PATH_NOT_READY ||
                ret == PICOQUIC_ERROR_MEMORY) {
                /* PICOQUIC_ERROR_MEMORY here is not a real allocation
                   failure - traced through picoquic_create_path ->
                   picoquic_find_avalaible_unique_path_id, which returns
                   this generic code when max_path_id_in_cnxid_lists blocks
                   it too, a value that grows dynamically as the peer's
                   NEW_CONNECTION_ID frames actually arrive rather than a
                   static negotiated limit. Reproducibly hit on the 2nd
                   extra path even after raising initial_max_path_id on
                   both sides, consistent with "not enough CIDs have
                   arrived yet" rather than a hard cap - retrying gives
                   the connection more round trips to receive them. */
                cb_ctx->client_alt_state[i] = 0;
                need_to_wait = 1;
                ret = 0;
            } else {
                fprintf(stderr, "mp_traffic: probe new path failed, code %d\n", ret);
            }
        } else {
            fprintf(stderr, "mp_traffic: new path added, total paths %d\n", cb_ctx->cnx_client->nb_paths);
        }
    }

    if (!need_to_wait) {
        cb_ctx->multipath_probe_done = 1;
    }
    return ret;
}

static int mp_client_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode,
    void* callback_ctx, void* callback_arg) {
    int ret = 0;
    mp_client_loop_cb_t* cb_ctx = (mp_client_loop_cb_t*)callback_ctx;
    (void)quic;

    switch (cb_mode) {
    case picoquic_packet_loop_ready: {
        picoquic_packet_loop_options_t* options = (picoquic_packet_loop_options_t*)callback_arg;
        options->provide_alt_port = 1;
        break;
    }
    case picoquic_packet_loop_after_receive:
        if (cb_ctx->cnx_client->is_multipath_enabled) {
            if (cb_ctx->multipath_initiated == 0) {
                int is_already_allowed = 0;
                cb_ctx->multipath_initiated = 1;
                if (picoquic_subscribe_new_path_allowed(cb_ctx->cnx_client, &is_already_allowed) == 0) {
                    if (is_already_allowed) {
                        ret = mp_client_create_additional_path(cb_ctx->cnx_client, cb_ctx);
                    }
                }
            }
            if (!cb_ctx->multipath_probe_done && cb_ctx->cnx_client->is_notified_that_path_is_allowed) {
                ret = mp_client_create_additional_path(cb_ctx->cnx_client, cb_ctx);
            }
        }
        break;
    case picoquic_packet_loop_alt_port:
        cb_ctx->alt_port = *((uint16_t*)callback_arg);
        break;
    default:
        break;
    }

    return ret;
}

int mp_run_client(mp_config_t* config) {
    mp_client_loop_cb_t loop_cb;
    memset(&loop_cb, 0, sizeof(loop_cb));

    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, "mp_traffic", NULL, NULL, NULL, NULL, NULL,
        picoquic_current_time(), NULL, NULL, NULL, 0);
    if (quic == NULL) {
        fprintf(stderr, "mp_traffic: could not create quic context\n");
        return -1;
    }

    picoquic_set_default_congestion_algorithm_by_name(quic, config->cc_algo);
    picoquic_set_default_multipath_option(quic, 1);
    /* Explicit, generous max_path_id rather than relying on picoquic's
       built-in default - Task 3's real-hardware test showed the foreign
       picoquicdemo peer's default capped us at 2 total paths (this app's
       own max_path_id_remote limit comes from the OTHER side's setting,
       so this call matters most on whichever side is the SERVER, but set
       symmetrically here too in case roles ever reverse). */
    picoquic_set_default_tp_value(quic, picoquic_tp_initial_max_path_id, MP_MAX_ALT_PATHS);
    if (config->qlog_dir[0] != 0) {
        picoquic_set_qlog(quic, config->qlog_dir);
    }

    int is_name = 0;
    int ret = picoquic_get_server_address(config->server_ip, config->port, &loop_cb.server_address, &is_name);
    if (ret != 0) {
        fprintf(stderr, "mp_traffic: could not resolve server address %s\n", config->server_ip);
        return -1;
    }

    /* ALPN "h3" (not a custom string) so this client can validate against
       the existing picoquicdemo server in Task 3's test - ALPN negotiation
       fails the handshake immediately if the client's offered protocol
       isn't in the server's supported list, and picoquicdemo only
       supports h3. Kept as h3 for the app's own server too (Task 4) rather
       than switching to a custom ALPN there and back - the string itself
       is just an identifier both sides need to agree on, nothing in this
       app depends on HTTP/3 semantics. */
    picoquic_cnx_t* cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&loop_cb.server_address, picoquic_current_time(), 0, "mp-traffic.test", "h3", 1);
    if (cnx == NULL) {
        fprintf(stderr, "mp_traffic: could not create connection\n");
        return -1;
    }

    picoquic_set_callback(cnx, mp_client_callback, config);

    ret = picoquic_start_client_cnx(cnx);
    if (ret != 0) {
        fprintf(stderr, "mp_traffic: could not start connection\n");
        return -1;
    }

    if (config->path_spec[0] != 0) {
        mp_parse_client_multipath_config(config->path_spec, loop_cb.client_alt_if,
            loop_cb.client_alt_address, loop_cb.server_alt_address, &loop_cb.nb_alt_paths,
            &loop_cb.server_address);
    }
    loop_cb.cnx_client = cnx;

    picoquic_packet_loop_param_t param;
    memset(&param, 0, sizeof(param));
    param.local_af = loop_cb.server_address.ss_family;
    if (config->path_spec[0] != 0) {
        param.local_port = (uint16_t)picoquic_uniform_random(30000) + 20000;
        param.extra_socket_required = 1;
    }

    ret = picoquic_packet_loop_v2(quic, &param, mp_client_loop_cb, &loop_cb);

    picoquic_free(quic);
    return ret;
}

/* --- Server connection setup. Confirmed against real picoquicdemo.c
   source that the server side needs no equivalent of the client's
   path-probing/driving logic - multipath path validation on accept is
   handled entirely inside the picoquic library engine, no application
   involvement required. This is genuinely simpler than Task 3's client
   side, not just simplified for this app's purposes. */
static int mp_server_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes,
    size_t length, picoquic_call_back_event_t event, void* callback_ctx, void* v_stream_ctx) {
    (void)stream_id; (void)bytes; (void)length; (void)callback_ctx; (void)v_stream_ctx;

    switch (event) {
    case picoquic_callback_ready:
        g_connection_ready_time = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));
        fprintf(stderr, "mp_traffic: server sees connection ready at %llu\n",
            (unsigned long long)g_connection_ready_time);
        break;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
        fprintf(stderr, "mp_traffic: server sees connection closed\n");
        break;
    default:
        break;
    }
    return 0;
}

int mp_run_server(mp_config_t* config) {
    /* Same cert path constraint discovered in the sweep harness (Phase 2):
       picoquic's default cert/key path (certs/cert.pem) is relative to
       process cwd, only exists under picoquic/certs/ - this binary must
       be run from inside picoquic/, or pass explicit cert/key paths. */
    picoquic_quic_t* quic = picoquic_create(8, "certs/cert.pem", "certs/key.pem", NULL, "h3",
        mp_server_callback, config, NULL, NULL, NULL,
        picoquic_current_time(), NULL, NULL, NULL, 0);
    if (quic == NULL) {
        fprintf(stderr, "mp_traffic: could not create server quic context\n");
        return -1;
    }

    picoquic_set_default_congestion_algorithm_by_name(quic, config->cc_algo);
    picoquic_set_default_multipath_option(quic, 1);
    /* Same explicit max_path_id fix as the client - don't rely on
       picoquic's built-in default, which Task 3's real-hardware test
       showed can be as low as 2 total paths against a foreign peer. */
    picoquic_set_default_tp_value(quic, picoquic_tp_initial_max_path_id, MP_MAX_ALT_PATHS);
    if (config->qlog_dir[0] != 0) {
        picoquic_set_qlog(quic, config->qlog_dir);
    }

    picoquic_packet_loop_param_t param;
    memset(&param, 0, sizeof(param));
    param.local_port = config->port;

    int ret = picoquic_packet_loop_v2(quic, &param, NULL, NULL);

    picoquic_free(quic);
    return ret;
}

int main(int argc, char** argv) {
    mp_config_t config;
    if (mp_parse_args(argc, argv, &config) != 0) {
        return 1;
    }

    if (config.is_server) {
        return mp_run_server(&config);
    } else {
        return mp_run_client(&config);
    }
}
