#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
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
#define MP_MAX_DATAGRAM_FRAME_SIZE 1400 /* comfortably above
    MP_VIDEO_CHUNK_BYTES plus frame overhead, within this rig's observed
    MTU range (~1252-1440 seen throughout this project) */

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

    /* atoi() on garbage or omitted values yields 0, and a negative
       --duration wraps via uint64_t arithmetic at the send sites - both
       happened to fail benign (immediate close) rather than being
       rejected, which is safe by accident, not by design. */
    if (config->duration_sec <= 0) {
        fprintf(stderr, "mp_traffic: --duration must be a positive number of seconds\n");
        return -1;
    }
    if (config->port <= 0 || config->port > 65535) {
        fprintf(stderr, "mp_traffic: port must be between 1 and 65535\n");
        return -1;
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
            /* Bound against MP_MAX_ALT_PATHS, not PICOQUIC_NB_PATH_TARGET
               directly - the arrays this index writes into (see
               mp_client_loop_cb_t below) are sized MP_MAX_ALT_PATHS,
               which is deliberately hardcoded rather than tied to
               picoquic's internal default (see that constant's own
               comment) precisely so it won't silently drift out of sync
               with an upstream change to PICOQUIC_NB_PATH_TARGET. */
            if (*nb_alt_paths >= MP_MAX_ALT_PATHS) {
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

/* Set by the connection-level close callback (mp_client_callback /
   mp_server_callback - only one runs per process), read by this side's
   packet-loop callback to request clean termination. Needed because
   closing the QUIC connection (picoquic_close) does not by itself stop
   picoquic_packet_loop_v2's socket loop - confirmed against picoquic's
   own sockloop.c, whose main loop only exits when a loop_callback
   returns a nonzero ret; there is no automatic exit-on-disconnect.
   Also set from mp_handle_sigint below, so it must be signal-safe:
   volatile sig_atomic_t, not plain int. */
static volatile sig_atomic_t g_should_exit = 0;

/* picoquic itself installs no signal handlers (confirmed: no signal()/
   sigaction() anywhere in sockloop.c or picoquicdemo.c) - SIGINT's
   default action just terminates the process, identically to SIGTERM,
   with no qlog flush either way. run-traffic-server.sh wraps this
   binary in `timeout -s INT` specifically so a stuck run's safety-net
   timeout gets a graceful shutdown instead of a bare kill; without this
   handler that comment would be describing behavior the code doesn't
   actually have. Routing SIGINT through the same g_should_exit flag
   both loop callbacks already check makes Ctrl-C during interactive use
   graceful too (closes the connection, flushes qlog) instead of an
   abrupt kill. */
static void mp_handle_sigint(int sig) {
    (void)sig;
    g_should_exit = 1;
}

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
/* Counts generated-vs-successfully-enqueued chunks. This is NOT a
   delivery signal: picoquic_queue_datagram_frame appends to an
   UNBOUNDED malloc'd list (confirmed against real picoquic source,
   picoquic_queue_misc_or_dg_frame in quicctx.c) - there is no "queue
   full" failure mode to catch under congestion, so a slow-draining
   connection just grows the backlog rather than rejecting new chunks,
   and this counter reads ~100% either way. The only real failures it
   catches are PICOQUIC_ERROR_DATAGRAM_TOO_LONG (chunk exceeds the
   negotiated/path MTU) and allocation failure. Real hardware runs
   confirmed this the hard way: BBR showed 100% here while client-side
   NIC TX bytes measured only ~28% of the 10Mbps target for that run -
   the NIC byte-counter deltas in run-traffic-client.sh remain the only
   trustworthy on-wire delivery signal, same as this project's
   methodology everywhere else. */
static uint64_t g_video_chunks_generated = 0;
static uint64_t g_video_chunks_queued = 0;

static void mp_client_send_video_if_due(picoquic_cnx_t* cnx, int duration_sec) {
    uint64_t now = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));

    if (!g_video_pacer_started) {
        mp_pacer_init(&g_video_pacer, now, MP_VIDEO_CHUNK_BYTES, MP_VIDEO_TARGET_BPS);
        g_video_pacer_started = 1;
    }

    uint64_t stop_time = g_connection_ready_time + (uint64_t)duration_sec * 1000000ULL;
    if (now >= stop_time) {
        fprintf(stderr, "mp_traffic: duration elapsed, closing (video: %llu/%llu chunks queued)\n",
            (unsigned long long)g_video_chunks_queued, (unsigned long long)g_video_chunks_generated);
        picoquic_close(cnx, 0);
        return;
    }

    while (mp_pacer_is_due(&g_video_pacer, now) && now < stop_time) {
        uint8_t chunk[MP_VIDEO_CHUNK_BYTES];
        memset(chunk, 0xAA, sizeof(chunk)); /* filler payload - content doesn't matter, see spec */
        g_video_chunks_generated++;
        if (picoquic_queue_datagram_frame(cnx, sizeof(chunk), chunk) == 0) {
            g_video_chunks_queued++;
        }
        mp_pacer_advance(&g_video_pacer);
    }

    picoquic_set_app_wake_time(cnx, mp_pacer_next_time(&g_video_pacer) < stop_time ?
        mp_pacer_next_time(&g_video_pacer) : stop_time);
}

static int mp_client_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes,
    size_t length, picoquic_call_back_event_t event, void* callback_ctx, void* v_stream_ctx) {
    (void)stream_id; (void)bytes; (void)length; (void)v_stream_ctx;

    switch (event) {
    case picoquic_callback_ready:
        g_connection_ready_time = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));
        fprintf(stderr, "mp_traffic: connection ready at %llu\n",
            (unsigned long long)g_connection_ready_time);
        mp_client_send_video_if_due(cnx, ((mp_config_t*)callback_ctx)->duration_sec);
        break;
    case picoquic_callback_app_wakeup:
        mp_client_send_video_if_due(cnx, ((mp_config_t*)callback_ctx)->duration_sec);
        break;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
        fprintf(stderr, "mp_traffic: connection closed\n");
        g_should_exit = 1;
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
                /* A non-retriable failure on ONE path (e.g. a flapping
                   link at probe time) used to kill the entire run: this
                   ret propagates through mp_client_loop_cb straight to
                   picoquic_packet_loop_v2, which treats any nonzero
                   return as fatal. For a long-running measurement
                   harness, finishing on fewer paths is strictly better
                   than aborting outright - log it and degrade instead.
                   client_alt_state[i] is already 1 (set above), so this
                   path won't be retried again. */
                fprintf(stderr, "mp_traffic: probe new path failed, code %d - continuing without it\n", ret);
                ret = 0;
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
    case picoquic_packet_loop_after_send:
        /* The only loop_callback event whose return value reliably
           survives to the outer loop's exit check - confirmed against
           real sockloop.c: picoquic_packet_loop_time_check's ret gets
           unconditionally overwritten by the timeout/qmux-check handling
           that runs right after it in the same iteration, so a
           termination request made there was silently discarded on
           every tick (that's why the previous fix compiled and ran but
           never actually exited). after_send is called last, once
           per iteration, with nothing after it to clobber ret. */
        if (g_should_exit) {
            ret = PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
        }
        break;
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

    /* picoquic_set_default_congestion_algorithm_by_name silently stores
       NULL for an unrecognized name - every congestion-control call site
       inside picoquic guards on non-NULL, so a typo'd -G doesn't crash,
       it just runs the whole connection with NO congestion control at
       all while exiting 0 with a complete qlog. For a testbed whose
       purpose is comparing CC algorithms, that's silent measurement
       corruption - check the name first via the public lookup API. */
    if (picoquic_get_congestion_algorithm(config->cc_algo) == NULL) {
        fprintf(stderr, "mp_traffic: unknown congestion algorithm '%s'\n", config->cc_algo);
        picoquic_free(quic);
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
    /* Datagram extension (RFC 9221) isn't enabled just by calling
       picoquic_queue_datagram_frame - it needs this transport parameter
       set, or the peer rejects incoming datagram frames outright with a
       FRAME_ENCODING_ERROR (confirmed on real hardware: server-side qlog
       showed exactly this, trigger_frame_type "datagram", closing the
       connection). Same shape of gap as max_path_id above - a feature
       that needs an explicit transport-parameter opt-in, not just using
       the sending API. */
    picoquic_set_default_tp_value(quic, picoquic_tp_max_datagram_frame_size, MP_MAX_DATAGRAM_FRAME_SIZE);
    if (config->qlog_dir[0] != 0) {
        picoquic_set_qlog(quic, config->qlog_dir);
    }

    int is_name = 0;
    int ret = picoquic_get_server_address(config->server_ip, config->port, &loop_cb.server_address, &is_name);
    if (ret != 0) {
        fprintf(stderr, "mp_traffic: could not resolve server address %s\n", config->server_ip);
        picoquic_free(quic);
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
        picoquic_free(quic);
        return -1;
    }

    picoquic_set_callback(cnx, mp_client_callback, config);

    ret = picoquic_start_client_cnx(cnx);
    if (ret != 0) {
        fprintf(stderr, "mp_traffic: could not start connection\n");
        picoquic_free(quic);
        return -1;
    }

    if (config->path_spec[0] != 0) {
        /* The parser's own return value only signals malloc failure -
           an invalid entry (bad IP, address-family mismatch) is silently
           skipped internally, leaving ret==0 with nb_alt_paths still 0.
           Checking nb_alt_paths too is what actually catches "the whole
           -A spec was garbage" instead of only the OOM case - for this
           app that distinction is the difference between the intended
           multipath run and a silent single-path fallback, worth a hard
           failure rather than continuing degraded and unnoticed. */
        if (mp_parse_client_multipath_config(config->path_spec, loop_cb.client_alt_if,
                loop_cb.client_alt_address, loop_cb.server_alt_address, &loop_cb.nb_alt_paths,
                &loop_cb.server_address) != 0 || loop_cb.nb_alt_paths == 0) {
            fprintf(stderr, "mp_traffic: could not parse -A path spec '%s'\n", config->path_spec);
            picoquic_free(quic);
            return -1;
        }
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

/* Control-like traffic generator: server -> client, on a regular
   reliable QUIC stream (not a datagram) - control commands need
   ordering/reliability that a datagram doesn't provide, the opposite
   tradeoff from the video-like traffic in Task 5. Same pacer module,
   same CBR approach, picoquic_add_to_stream instead of
   picoquic_queue_datagram_frame. */
#define MP_CONTROL_CHUNK_BYTES 125
#define MP_CONTROL_TARGET_BPS 1000000ULL
#define MP_CONTROL_STREAM_ID 1 /* server-initiated bidirectional stream */

static mp_pacer_t g_control_pacer;
static int g_control_pacer_started = 0;
/* Same generated-vs-queued accounting as the video side - see the
   comment on g_video_chunks_generated/g_video_chunks_queued. */
static uint64_t g_control_chunks_generated = 0;
static uint64_t g_control_chunks_queued = 0;

static void mp_server_send_control_if_due(picoquic_cnx_t* cnx, int duration_sec) {
    uint64_t now = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));

    if (!g_control_pacer_started) {
        mp_pacer_init(&g_control_pacer, now, MP_CONTROL_CHUNK_BYTES, MP_CONTROL_TARGET_BPS);
        g_control_pacer_started = 1;
        /* No picoquic_mark_active_stream call here - picoquic.h documents
           that picoquic_add_to_stream (below) automatically supersedes
           any active-stream mark, and there's no
           picoquic_callback_prepare_to_send handler in this app for it
           to matter to, so the call was dead code. */
    }

    uint64_t stop_time = g_connection_ready_time + (uint64_t)duration_sec * 1000000ULL;
    if (now >= stop_time) {
        fprintf(stderr, "mp_traffic: duration elapsed, closing (control: %llu/%llu chunks queued)\n",
            (unsigned long long)g_control_chunks_queued, (unsigned long long)g_control_chunks_generated);
        picoquic_close(cnx, 0);
        return;
    }

    while (mp_pacer_is_due(&g_control_pacer, now) && now < stop_time) {
        uint8_t chunk[MP_CONTROL_CHUNK_BYTES];
        memset(chunk, 0xBB, sizeof(chunk)); /* filler payload */
        g_control_chunks_generated++;
        if (picoquic_add_to_stream(cnx, MP_CONTROL_STREAM_ID, chunk, sizeof(chunk), 0) == 0) {
            g_control_chunks_queued++;
        }
        mp_pacer_advance(&g_control_pacer);
    }

    picoquic_set_app_wake_time(cnx, mp_pacer_next_time(&g_control_pacer) < stop_time ?
        mp_pacer_next_time(&g_control_pacer) : stop_time);
}

/* --- Server connection setup. Confirmed against real picoquicdemo.c
   source that the server side needs no equivalent of the client's
   path-probing/driving logic - multipath path validation on accept is
   handled entirely inside the picoquic library engine, no application
   involvement required. This is genuinely simpler than Task 3's client
   side, not just simplified for this app's purposes. */
static int mp_server_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes,
    size_t length, picoquic_call_back_event_t event, void* callback_ctx, void* v_stream_ctx) {
    (void)stream_id; (void)bytes; (void)length; (void)v_stream_ctx;

    switch (event) {
    case picoquic_callback_ready:
        g_connection_ready_time = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));
        fprintf(stderr, "mp_traffic: server sees connection ready at %llu\n",
            (unsigned long long)g_connection_ready_time);
        mp_server_send_control_if_due(cnx, ((mp_config_t*)callback_ctx)->duration_sec);
        break;
    case picoquic_callback_app_wakeup:
        mp_server_send_control_if_due(cnx, ((mp_config_t*)callback_ctx)->duration_sec);
        break;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
        fprintf(stderr, "mp_traffic: server sees connection closed\n");
        g_should_exit = 1;
        break;
    default:
        break;
    }
    return 0;
}

/* Unlike the client, the server had no packet-loop callback at all
   (picoquic_packet_loop_v2 was called with NULL, NULL) - fine while the
   only stop condition was manual Ctrl-C, but picoquic_close() alone
   never makes picoquic_packet_loop_v2 return, so once mp_server_callback
   sets g_should_exit this is the hook that actually terminates the loop. */
static int mp_server_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode,
    void* callback_ctx, void* callback_arg) {
    int ret = 0;
    (void)quic; (void)callback_ctx; (void)callback_arg;

    switch (cb_mode) {
    case picoquic_packet_loop_after_send:
        /* after_send is the only loop_callback event whose return value
           reliably survives to the outer loop's exit check - see the
           matching comment on mp_client_loop_cb for why time_check
           doesn't work (its ret gets clobbered by the timeout/qmux-check
           handling in the same iteration). */
        if (g_should_exit) {
            ret = PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
        }
        break;
    default:
        break;
    }
    return ret;
}

int mp_run_server(mp_config_t* config) {
    /* Same cert path constraint discovered in the sweep harness (Phase 2):
       picoquic's default cert/key path (certs/cert.pem) is relative to
       process cwd, only exists under picoquic/certs/ - this binary must
       be run from inside picoquic/, or pass explicit cert/key paths. */
    /* Connection cap of 1, not 8: g_connection_ready_time, g_control_pacer
       and g_should_exit are process-wide globals (this app is a one-shot
       single-connection traffic generator, not a real multi-client
       server - see the spec's stated scope), so a second concurrent
       connection would find the control pacer already "started" and
       burst-flush stale state, and one connection's close would
       terminate the packet loop out from under the other. Enforcing the
       limit here makes that constraint real instead of assumed. */
    picoquic_quic_t* quic = picoquic_create(1, "certs/cert.pem", "certs/key.pem", NULL, "h3",
        mp_server_callback, config, NULL, NULL, NULL,
        picoquic_current_time(), NULL, NULL, NULL, 0);
    if (quic == NULL) {
        fprintf(stderr, "mp_traffic: could not create server quic context\n");
        return -1;
    }

    if (picoquic_get_congestion_algorithm(config->cc_algo) == NULL) {
        fprintf(stderr, "mp_traffic: unknown congestion algorithm '%s'\n", config->cc_algo);
        picoquic_free(quic);
        return -1;
    }
    picoquic_set_default_congestion_algorithm_by_name(quic, config->cc_algo);
    picoquic_set_default_multipath_option(quic, 1);
    /* Same explicit max_path_id fix as the client - don't rely on
       picoquic's built-in default, which Task 3's real-hardware test
       showed can be as low as 2 total paths against a foreign peer. */
    picoquic_set_default_tp_value(quic, picoquic_tp_initial_max_path_id, MP_MAX_ALT_PATHS);
    /* Datagram extension (RFC 9221) isn't enabled just by calling
       picoquic_queue_datagram_frame - it needs this transport parameter
       set, or the peer rejects incoming datagram frames outright with a
       FRAME_ENCODING_ERROR (confirmed on real hardware: server-side qlog
       showed exactly this, trigger_frame_type "datagram", closing the
       connection). Same shape of gap as max_path_id above - a feature
       that needs an explicit transport-parameter opt-in, not just using
       the sending API. */
    picoquic_set_default_tp_value(quic, picoquic_tp_max_datagram_frame_size, MP_MAX_DATAGRAM_FRAME_SIZE);
    if (config->qlog_dir[0] != 0) {
        picoquic_set_qlog(quic, config->qlog_dir);
    }

    picoquic_packet_loop_param_t param;
    memset(&param, 0, sizeof(param));
    param.local_port = config->port;

    int ret = picoquic_packet_loop_v2(quic, &param, mp_server_loop_cb, NULL);

    picoquic_free(quic);
    return ret;
}

int main(int argc, char** argv) {
    mp_config_t config;
    if (mp_parse_args(argc, argv, &config) != 0) {
        return 1;
    }

    /* Both picoquic_get_congestion_algorithm and
       picoquic_set_default_congestion_algorithm_by_name look up names in
       a process-wide registry (picoquic_congestion_control_algorithms in
       quicctx.c) that starts NULL/empty - nothing populates it
       automatically. picoquicdemo.c calls this exact function at the top
       of its own main() (picoquicfirst/picoquicdemo.c) before parsing
       -G; this app never did, which meant every -G value including the
       default "cubic" silently resolved to NULL and every run this
       session had congestion control fully disabled, not "cubic" - the
       new validation added after the final review didn't just add a
       safety check, it surfaced this pre-existing bug immediately by
       rejecting the very first run. */
    picoquic_register_all_congestion_control_algorithms();
    signal(SIGINT, mp_handle_sigint);

    if (config.is_server) {
        return mp_run_server(&config);
    } else {
        return mp_run_client(&config);
    }
}
