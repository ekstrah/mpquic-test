# mp_traffic Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `mp_traffic`, a standalone multipath QUIC app that generates the source paper's traffic model — a continuous ~10Mbps video-like datagram stream from client (vehicle) to server (ground station), and a continuous ~1Mbps control-like reliable stream from server to client — instead of the single bulk-file transfer every prior test in this project used.

**Architecture:** New `traffic-app/` directory (sibling to `picoquic/`, not inside it) forking `picoquicdemo.c`'s proven connection/multipath setup, with a small standalone CBR pacing module reused identically by both traffic directions, adapted from the proven pacing/wake-time pattern already shipped in picoquic's own `picohttp/quicperf.c`.

**Tech Stack:** C (C99), CMake, picoquic's raw transport API (no HTTP/3 — this app doesn't need it), OpenSSL/picotls (inherited via picoquic's own CMake build).

**Spec:** `docs/superpowers/specs/2026-08-28-mp-traffic-app-design.md`

## Global Constraints

- Multipath: must comply with `draft-ietf-quic-multipath-20` (this picoquic build's tracked version, confirmed via `picoquic_internal.h` frame-type comments) — i.e., use picoquic's existing multipath API (`-M`, path declaration) exactly as `picoquicdemo` does, no custom multipath logic.
- Datagrams: RFC 9221 (finalized, distinct maturity from multipath — state this distinction in any output/report this app produces).
- Traffic direction: client (vehicle) sends the 10Mbps video-like stream via datagrams; server (ground station) sends the 1Mbps control-like stream via a reliable QUIC stream. Do not swap these.
- Both traffic types are constant-bitrate (fixed chunk size, fixed interval) — no variable-bitrate logic in this plan.
- Per-link traffic attribution stays external to the app (client-side NIC byte counters, same as `run-client-sweep.sh`) — do not add per-path instrumentation inside `mp_traffic`.
- `picoquic/` checkout must not be modified — link against it from the sibling `traffic-app/` directory only.
- Stop condition is wall-clock `--duration <seconds>` from connection-ready, with an explicit `picoquic_close()` on both sides — no implicit "peer stopped, assume done" logic.

---

### Task 1: CBR pacing module (pure C, no picoquic dependency)

**Files:**
- Create: `traffic-app/pacer.h`
- Create: `traffic-app/pacer.c`
- Test: `traffic-app/test_pacer.c`

**Interfaces:**
- Produces: `mp_pacer_t` struct, `mp_pacer_init()`, `mp_pacer_is_due()`, `mp_pacer_next_time()`, `mp_pacer_advance()` — used by Task 5 (client datagram generator) and Task 6 (server stream generator) to decide when to send the next chunk. All times are `uint64_t` microseconds, matching picoquic's own time convention (`picoquic_current_time()`/`picoquic_get_quic_time()` both return microseconds).

This is pure math with no picoquic or network dependency, so it's the one part of this plan that can be built and tested locally with a plain C compiler, independent of the remote rig. The formula (cumulative `start_time + N*interval`, not `last_time + interval` repeated) is deliberately the same pattern `quicperf.c` already uses in picoquic's own codebase, specifically because addition-based accumulation drifts under rounding error over a long run and this doesn't.

- [ ] **Step 1: Write the failing test**

```c
// traffic-app/test_pacer.c
#include <assert.h>
#include <stdio.h>
#include "pacer.h"

int main(void) {
    mp_pacer_t p;

    /* 1200-byte chunks at 10,000,000 bps -> interval = 1200*8*1e6/10e6 = 960us */
    mp_pacer_init(&p, 1000000, 1200, 10000000);
    assert(p.interval_us == 960);
    assert(mp_pacer_is_due(&p, 1000000) == 1);   /* first chunk due immediately at start_time */
    assert(mp_pacer_is_due(&p, 999999) == 0);    /* not due before start_time */
    assert(mp_pacer_next_time(&p) == 1000000);

    mp_pacer_advance(&p);
    assert(p.nb_chunks_sent == 1);
    assert(mp_pacer_next_time(&p) == 1000960);
    assert(mp_pacer_is_due(&p, 1000959) == 0);
    assert(mp_pacer_is_due(&p, 1000960) == 1);

    /* 125-byte chunks at 1,000,000 bps -> interval = 125*8*1e6/1e6 = 1000us */
    mp_pacer_t q;
    mp_pacer_init(&q, 0, 125, 1000000);
    assert(q.interval_us == 1000);

    /* No drift after many advances: next_time == start_time + N*interval exactly */
    mp_pacer_t r;
    mp_pacer_init(&r, 500, 1200, 10000000);
    for (int i = 0; i < 1000; i++) {
        mp_pacer_advance(&r);
    }
    assert(mp_pacer_next_time(&r) == 500 + 1000ULL * 960ULL);

    printf("all pacer tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails (files don't exist yet)**

Run: `cd traffic-app && gcc -o test_pacer test_pacer.c pacer.c -I. && ./test_pacer`
Expected: FAIL — compiler error, `pacer.h`/`pacer.c` not found.

- [ ] **Step 3: Write the pacer header**

```c
// traffic-app/pacer.h
#ifndef MP_PACER_H
#define MP_PACER_H

#include <stdint.h>

typedef struct st_mp_pacer_t {
    uint64_t start_time;      /* microseconds, matches picoquic_current_time() convention */
    uint64_t interval_us;     /* time between chunks to hit the target rate */
    uint64_t nb_chunks_sent;
} mp_pacer_t;

/* chunk_bytes: size of each send. target_bps: desired bits per second. */
void mp_pacer_init(mp_pacer_t* pacer, uint64_t start_time, uint64_t chunk_bytes, uint64_t target_bps);

/* 1 if a chunk is due to be sent at time `now`, 0 otherwise. */
int mp_pacer_is_due(const mp_pacer_t* pacer, uint64_t now);

/* Absolute time (microseconds) the next chunk is due - use this to schedule the next wake. */
uint64_t mp_pacer_next_time(const mp_pacer_t* pacer);

/* Call after actually sending a chunk. */
void mp_pacer_advance(mp_pacer_t* pacer);

#endif
```

- [ ] **Step 4: Write the pacer implementation**

```c
// traffic-app/pacer.c
#include "pacer.h"

void mp_pacer_init(mp_pacer_t* pacer, uint64_t start_time, uint64_t chunk_bytes, uint64_t target_bps) {
    pacer->start_time = start_time;
    pacer->interval_us = (chunk_bytes * 8ULL * 1000000ULL) / target_bps;
    pacer->nb_chunks_sent = 0;
}

uint64_t mp_pacer_next_time(const mp_pacer_t* pacer) {
    return pacer->start_time + pacer->nb_chunks_sent * pacer->interval_us;
}

int mp_pacer_is_due(const mp_pacer_t* pacer, uint64_t now) {
    return now >= mp_pacer_next_time(pacer);
}

void mp_pacer_advance(mp_pacer_t* pacer) {
    pacer->nb_chunks_sent += 1;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd traffic-app && gcc -o test_pacer test_pacer.c pacer.c -I. && ./test_pacer`
Expected: PASS — prints `all pacer tests passed`, exit code 0.

- [ ] **Step 6: Commit**

```bash
git add traffic-app/pacer.h traffic-app/pacer.c traffic-app/test_pacer.c
git commit -m "Add CBR pacing module for mp_traffic, with local unit tests"
```

---

### Task 2: Project scaffold and CLI argument parsing

**Files:**
- Create: `traffic-app/CMakeLists.txt`
- Create: `traffic-app/mp_traffic.c`

**Interfaces:**
- Consumes: nothing yet from Task 1 (wired in at Task 5/6).
- Produces: a `mp_config_t` struct and `mp_parse_args()` function that later tasks extend — `is_server`, `port`, `server_ip`, `path_spec` (the raw `-A` string), `cc_algo`, `duration_sec`.

This task's verification requires the real box (first step that links against the actual picoquic build) — everything before this was pure local C.

- [ ] **Step 1: Write CMakeLists.txt**

```cmake
# traffic-app/CMakeLists.txt
cmake_minimum_required(VERSION 3.5)
project(mp_traffic C)

set(CMAKE_C_STANDARD 99)

# Reuse the sibling picoquic checkout's own CMake project rather than
# re-deriving its OpenSSL/picotls dependency resolution by hand - this
# is the standard CMake pattern for depending on a sibling project.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../picoquic ${CMAKE_CURRENT_BINARY_DIR}/picoquic-build)

add_executable(mp_traffic mp_traffic.c pacer.c)
target_include_directories(mp_traffic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../picoquic/picoquic)
target_link_libraries(mp_traffic PRIVATE picoquic-log picoquic-core ${PTLS_LIBRARIES} ${OPENSSL_LIBRARIES})

add_executable(test_pacer test_pacer.c pacer.c)
```

- [ ] **Step 2: Write mp_traffic.c with CLI parsing only (no picoquic connection yet)**

```c
// traffic-app/mp_traffic.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct st_mp_config_t {
    int is_server;
    int port;
    char server_ip[64];
    char path_spec[256];   /* raw -A value, parsed further in Task 3 */
    char cc_algo[32];
    int duration_sec;
} mp_config_t;

static void mp_usage(const char* prog) {
    fprintf(stderr, "Server: %s -p <port> [-G <cc_algo>]\n", prog);
    fprintf(stderr, "Client: %s -A <path_spec> [-G <cc_algo>] --duration <seconds> <server_ip> <port>\n", prog);
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

    while ((opt = getopt_long(argc, argv, "p:A:G:", long_opts, NULL)) != -1) {
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

int main(int argc, char** argv) {
    mp_config_t config;
    if (mp_parse_args(argc, argv, &config) != 0) {
        return 1;
    }

    if (config.is_server) {
        printf("server mode: port=%d cc=%s\n", config.port, config.cc_algo);
    } else {
        printf("client mode: server=%s port=%d path_spec=%s cc=%s duration=%d\n",
            config.server_ip, config.port, config.path_spec, config.cc_algo, config.duration_sec);
    }
    return 0;
}
```

Add `#include <getopt.h>` to the top of `mp_traffic.c` alongside the existing includes.

- [ ] **Step 3: Build on the real box (server or client, doesn't matter which for this step)**

```bash
cd mpquic-test/traffic-app
cmake .
make
```

Expected: builds cleanly, produces `mp_traffic` and `test_pacer` binaries. If `add_subdirectory` fails because `picoquic/`'s own CMakeLists assumes it's the top-level project (some projects guard on `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR`), the fallback is `find_library(PICOQUIC_CORE_LIB picoquic-core PATHS ../picoquic)` + `find_library(PICOQUIC_LOG_LIB picoquic-log PATHS ../picoquic)` + `find_package(OpenSSL REQUIRED)` against the already-built `.a` files instead of re-running picoquic's full CMake project — note which one was needed once you know, since it affects Task 3 onward only if the include/link approach changes, not the app logic itself.

- [ ] **Step 4: Smoke-test argument parsing**

```bash
./mp_traffic -p 4433 -G cubic
./mp_traffic -A "172.16.2.2/4,172.16.3.2/3" -G cubic --duration 15 10.99.0.1 4433
```

Expected: prints back the parsed config matching what was passed in (server line shows `port=4433 cc=cubic`; client line shows the full path_spec, cc, and duration=15). No connection is attempted yet.

- [ ] **Step 5: Commit**

```bash
git add traffic-app/CMakeLists.txt traffic-app/mp_traffic.c
git commit -m "Scaffold mp_traffic project with CLI argument parsing"
```

---

### Task 3: Client connection setup (multipath, no traffic yet)

**Files:**
- Modify: `traffic-app/mp_traffic.c`

**Interfaces:**
- Consumes: `mp_config_t` from Task 2.
- Produces: `mp_client_callback()` (the connection's event handler, extended in Task 5), `mp_run_client()`.

This task's client is tested against the **existing, already-proven** `picoquicdemo` server (not our own server yet, which doesn't exist until Task 4) — isolating whether the new client-side code is correct before adding a second new, untested component.

- [ ] **Step 1: Add the client callback and connection setup**

```c
// Add to mp_traffic.c, above main()
#include "picoquic.h"
#include "picoquic_utils.h"

static uint64_t g_connection_ready_time = 0;

static int mp_client_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes,
    size_t length, picoquic_call_back_event_t event, void* callback_ctx, void* v_stream_ctx) {
    (void)stream_id; (void)bytes; (void)length; (void)callback_ctx; (void)v_stream_ctx;

    switch (event) {
    case picoquic_callback_ready:
        g_connection_ready_time = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));
        fprintf(stderr, "mp_traffic: connection ready at %llu\n",
            (unsigned long long)g_connection_ready_time);
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

int mp_run_client(mp_config_t* config) {
    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, "mp_traffic", NULL, NULL, NULL, NULL, NULL,
        picoquic_current_time(), NULL, NULL, NULL, 0);
    if (quic == NULL) {
        fprintf(stderr, "mp_traffic: could not create quic context\n");
        return -1;
    }

    picoquic_set_default_congestion_algorithm_by_name(quic, config->cc_algo);
    picoquic_set_default_multipath_option(quic, 1);

    struct sockaddr_storage server_address;
    int is_name = 0;
    int ret = picoquic_get_server_address(config->server_ip, config->port, &server_address, &is_name);
    if (ret != 0) {
        fprintf(stderr, "mp_traffic: could not resolve server address %s\n", config->server_ip);
        return -1;
    }

    picoquic_cnx_t* cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&server_address, picoquic_current_time(), 0, "mp-traffic.test", "mp-traffic", 1);
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

    /* -A path declaration uses the identical parsing picoquicdemo.c already
       validates all session - reuse via picoquic_config for consistency
       rather than hand-parsing config->path_spec. */
    picoquic_config_t demo_config;
    picoquic_config_init(&demo_config);
    demo_config.multipath_alt_config = config->path_spec;

    struct sockaddr_storage client_addresses[PICOQUIC_MAX_PATHS];
    int client_ifindex[PICOQUIC_MAX_PATHS];
    struct sockaddr_storage alt_server_addresses[PICOQUIC_MAX_PATHS];
    int nb_alt_paths = 0;
    ret = picoquic_config_parse_alt_config(&demo_config, client_addresses, client_ifindex,
        alt_server_addresses, &nb_alt_paths, PICOQUIC_MAX_PATHS);
    if (ret == 0) {
        for (int i = 0; i < nb_alt_paths; i++) {
            struct sockaddr* target = (alt_server_addresses[i].ss_family != 0) ?
                (struct sockaddr*)&alt_server_addresses[i] : (struct sockaddr*)&server_address;
            picoquic_probe_new_path_ex(cnx, target, (struct sockaddr*)&client_addresses[i],
                client_ifindex[i], picoquic_current_time(), 0);
        }
    }

    picoquic_packet_loop_param_t param;
    memset(&param, 0, sizeof(param));
    param.local_af = server_address.ss_family;

    ret = picoquic_packet_loop_v2(quic, &param, NULL, NULL);

    picoquic_free(quic);
    return ret;
}
```

**Note for the implementer:** `picoquic_config_parse_alt_config` is the function name inferred from Phase 0 research on how `picoquicdemo.c` parses `-A`, but its exact name/signature was not directly re-verified against source in this planning pass (the Phase 0 research confirmed the `-A` *syntax* and that it drives `picoquic_probe_new_path_ex` calls, not this exact parsing function's name). Before trusting this block, grep the actual installed `picoquic/picoquicfirst/picoquicdemo.c` for how it handles `config.multipath_alt_config` (search for that field name) and copy the *real* parsing call it uses — this is the one piece of this task most likely to need a signature correction against the real source once you're on the box with the file in front of you.

- [ ] **Step 2: Wire up main() to call mp_run_client for client mode**

```c
// Replace the client branch in main()'s if/else with:
} else {
    return mp_run_client(&config);
}
```

- [ ] **Step 3: Build**

```bash
cd mpquic-test/traffic-app
cmake . && make
```

Expected: builds cleanly, or fails on the `picoquic_config_parse_alt_config` call — if so, fix per the note in Step 1 (grep `picoquicdemo.c` for the real function name/signature) before proceeding.

- [ ] **Step 4: Test against the existing, proven picoquicdemo server**

**[SERVER]** (the same command used all session, no changes):
```bash
cd mpquic-test/picoquic
./picoquicdemo -w ../www -p 4433 -M -q ../qlogs
```

**[CLIENT]** (the new binary):
```bash
cd mpquic-test/traffic-app
./mp_traffic -A "172.16.2.2/4,172.16.3.2/3" -G cubic --duration 15 10.99.0.1 4433
```

Expected: `mp_traffic: connection ready at <some number>` printed to stderr, then the process either hangs in the packet loop (expected — there's no traffic or stop condition yet, this task doesn't implement Task 7's duration-based stop) or exits if the server's `-1`-equivalent behavior isn't set (the existing server command above doesn't include `-1`, so it stays up; `Ctrl-C` the client manually after confirming "connection ready" printed). Confirm via the existing proven signal: check the server's own log for `Accept enable multipath: 1.` and use `ip -s link show` deltas on all 3 client interfaces to confirm all 3 paths actually carried packets (the same validation used throughout Phase 1).

- [ ] **Step 5: Commit**

```bash
git add traffic-app/mp_traffic.c
git commit -m "Add mp_traffic client connection setup with multipath"
```

---

### Task 4: Server connection setup (multipath, no traffic yet)

**Files:**
- Modify: `traffic-app/mp_traffic.c`

**Interfaces:**
- Consumes: `mp_config_t` from Task 2.
- Produces: `mp_server_callback()` (extended in Task 6), `mp_run_server()`.

Tested first against the existing proven `picoquicdemo` client (isolating server correctness), then both new binaries together.

- [ ] **Step 1: Add the server callback and setup**

```c
// Add to mp_traffic.c
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
    picoquic_quic_t* quic = picoquic_create(8, "certs/cert.pem", "certs/key.pem", NULL, "mp-traffic",
        mp_server_callback, config, NULL, NULL, NULL,
        picoquic_current_time(), NULL, NULL, NULL, 0);
    if (quic == NULL) {
        fprintf(stderr, "mp_traffic: could not create server quic context\n");
        return -1;
    }

    picoquic_set_default_congestion_algorithm_by_name(quic, config->cc_algo);
    picoquic_set_default_multipath_option(quic, 1);

    picoquic_packet_loop_param_t param;
    memset(&param, 0, sizeof(param));
    param.local_port = config->port;

    int ret = picoquic_packet_loop_v2(quic, &param, NULL, NULL);

    picoquic_free(quic);
    return ret;
}
```

- [ ] **Step 2: Wire up main()'s server branch**

```c
if (config.is_server) {
    return mp_run_server(&config);
}
```

- [ ] **Step 3: Build**

```bash
cd mpquic-test/traffic-app
cmake . && make
```

- [ ] **Step 4: Test our new server against the existing proven picoquicdemo client**

**[SERVER]** (new binary, run from inside `picoquic/` per the cert-path constraint):
```bash
cd mpquic-test/picoquic
../traffic-app/mp_traffic -p 4433 -G cubic
```

**[CLIENT]** (existing, proven binary):
```bash
cd mpquic-test/picoquic
./picoquicdemo -M -n test.example.com -A "172.16.2.2/4,172.16.3.2/3" 10.99.0.1 4433 /testfile.bin
```

Expected: the new server prints `mp_traffic: server sees connection ready`. The client will fail the actual GET (our new server has no HTTP/3 handler — `picoquicdemo`'s GET is HTTP/3-specific, and this server doesn't implement that), but the connection and multipath negotiation should succeed before that failure, which is what this step verifies.

- [ ] **Step 5: Test both new binaries together**

**[SERVER]:**
```bash
cd mpquic-test/picoquic
../traffic-app/mp_traffic -p 4433 -G cubic
```

**[CLIENT]:**
```bash
cd mpquic-test/traffic-app
./mp_traffic -A "172.16.2.2/4,172.16.3.2/3" -G cubic --duration 15 10.99.0.1 4433
```

Expected: both sides print "connection ready." `Ctrl-C` both manually — no stop condition or traffic exists yet (Tasks 5-7).

- [ ] **Step 6: Commit**

```bash
git add traffic-app/mp_traffic.c
git commit -m "Add mp_traffic server connection setup with multipath"
```

---

### Task 5: Client-side datagram video-like generator (10Mbps, CBR)

**Files:**
- Modify: `traffic-app/mp_traffic.c`

**Interfaces:**
- Consumes: `mp_pacer_t`/`mp_pacer_*` from Task 1; `mp_client_callback` from Task 3.
- Produces: video traffic flowing client→server via datagrams once the connection is ready.

- [ ] **Step 1: Add the pacer to the client and wire it to the wakeup event**

```c
// Add near the top of mp_traffic.c
#include "pacer.h"

#define MP_VIDEO_CHUNK_BYTES 1200
#define MP_VIDEO_TARGET_BPS 10000000ULL

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
```

- [ ] **Step 2: Call it from the client callback on the events that mean "we can send now"**

```c
// In mp_client_callback, extend the switch:
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
```

- [ ] **Step 3: Build**

```bash
cd mpquic-test/traffic-app
cmake . && make
```

- [ ] **Step 4: Test against our own server (from Task 4), watching qlog for datagram frames**

**[SERVER]:**
```bash
cd mpquic-test/picoquic
../traffic-app/mp_traffic -p 4433 -G cubic -q ../qlogs_mptraffic
```

(If `-q` isn't accepted yet — Task 2's arg parser doesn't have it — add a `-q <dir>` option to `mp_config_t`/`mp_parse_args` mirroring the existing `port`/`cc_algo` fields, and pass it through to `picoquic_create`'s qlog-directory-equivalent setup, matching how `picoquicdemo.c` wires `-q` today.)

**[CLIENT]:**
```bash
cd mpquic-test/traffic-app
./mp_traffic -A "172.16.2.2/4,172.16.3.2/3" -G cubic --duration 5 10.99.0.1 4433
```

Wait ~5 seconds, `Ctrl-C` both sides (Task 7 adds automatic stop). Expected: `grep -c '"frame_type": "datagram"' qlogs_mptraffic/*.qlog` (or the client-side qlog if added) shows roughly `duration_sec * (10000000/(1200*8))` ≈ `duration_sec * 1042` datagram frames — check the actual count is in that ballpark, not exact (real send timing has jitter).

- [ ] **Step 5: Commit**

```bash
git add traffic-app/mp_traffic.c
git commit -m "Add CBR datagram video-like traffic generator to mp_traffic client"
```

---

### Task 6: Server-side stream control-like generator (1Mbps, CBR)

**Files:**
- Modify: `traffic-app/mp_traffic.c`

**Interfaces:**
- Consumes: `mp_pacer_t`/`mp_pacer_*` from Task 1; `mp_server_callback` from Task 4.
- Produces: control traffic flowing server→client via a reliable stream once the connection is ready.

- [ ] **Step 1: Add the pacer to the server and a stream-active callback handler**

```c
#define MP_CONTROL_CHUNK_BYTES 125
#define MP_CONTROL_TARGET_BPS 1000000ULL
#define MP_CONTROL_STREAM_ID 1  /* server-initiated bidirectional stream */

static mp_pacer_t g_control_pacer;
static int g_control_pacer_started = 0;

static void mp_server_send_control_if_due(picoquic_cnx_t* cnx) {
    uint64_t now = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));

    if (!g_control_pacer_started) {
        mp_pacer_init(&g_control_pacer, now, MP_CONTROL_CHUNK_BYTES, MP_CONTROL_TARGET_BPS);
        g_control_pacer_started = 1;
        picoquic_mark_active_stream(cnx, MP_CONTROL_STREAM_ID, 1, NULL);
    }

    while (mp_pacer_is_due(&g_control_pacer, now)) {
        uint8_t chunk[MP_CONTROL_CHUNK_BYTES];
        memset(chunk, 0xBB, sizeof(chunk)); /* filler payload */
        picoquic_add_to_stream(cnx, MP_CONTROL_STREAM_ID, chunk, sizeof(chunk), 0);
        mp_pacer_advance(&g_control_pacer);
    }

    picoquic_set_app_wake_time(cnx, mp_pacer_next_time(&g_control_pacer));
}
```

- [ ] **Step 2: Call it from the server callback**

```c
// In mp_server_callback, extend the switch (mirrors the client's structure from Task 5):
switch (event) {
case picoquic_callback_ready:
    g_connection_ready_time = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));
    fprintf(stderr, "mp_traffic: server sees connection ready at %llu\n",
        (unsigned long long)g_connection_ready_time);
    mp_server_send_control_if_due(cnx);
    break;
case picoquic_callback_app_wakeup:
    mp_server_send_control_if_due(cnx);
    break;
case picoquic_callback_close:
case picoquic_callback_application_close:
    fprintf(stderr, "mp_traffic: server sees connection closed\n");
    break;
default:
    break;
}
```

- [ ] **Step 3: Build**

```bash
cd mpquic-test/traffic-app
cmake . && make
```

- [ ] **Step 4: Run both new binaries together, confirm both traffic directions**

**[SERVER]:**
```bash
cd mpquic-test/picoquic
../traffic-app/mp_traffic -p 4433 -G cubic -q ../qlogs_mptraffic_server
```

**[CLIENT]:**
```bash
cd mpquic-test/traffic-app
./mp_traffic -A "172.16.2.2/4,172.16.3.2/3" -G cubic --duration 5 10.99.0.1 4433
```

Wait ~5s, `Ctrl-C` both. Check the server-side qlog for both traffic types now present: `grep -c '"frame_type": "stream"' qlogs_mptraffic_server/*.qlog` should show roughly `duration_sec * (1000000/(125*8))` ≈ `duration_sec * 1000` stream-data events (server sending), and the datagram count from Task 5 should still be present too (client sending, visible in a client-side qlog if `-q` was added to the client's args as well).

- [ ] **Step 5: Commit**

```bash
git add traffic-app/mp_traffic.c
git commit -m "Add CBR stream control-like traffic generator to mp_traffic server"
```

---

### Task 7: Duration-based stop condition

**Files:**
- Modify: `traffic-app/mp_traffic.c`

**Interfaces:**
- Consumes: `config->duration_sec` from Task 2; `g_connection_ready_time` set in Tasks 3/4.

- [ ] **Step 1: Add a stop check to both pacers' wakeup handlers**

```c
// Replace mp_client_send_video_if_due's body with a duration check at the top:
static void mp_client_send_video_if_due(picoquic_cnx_t* cnx, int duration_sec) {
    uint64_t now = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));

    if (!g_video_pacer_started) {
        mp_pacer_init(&g_video_pacer, now, MP_VIDEO_CHUNK_BYTES, MP_VIDEO_TARGET_BPS);
        g_video_pacer_started = 1;
    }

    uint64_t stop_time = g_connection_ready_time + (uint64_t)duration_sec * 1000000ULL;
    if (now >= stop_time) {
        fprintf(stderr, "mp_traffic: duration elapsed, closing\n");
        picoquic_close(cnx, 0);
        return;
    }

    while (mp_pacer_is_due(&g_video_pacer, now) && now < stop_time) {
        uint8_t chunk[MP_VIDEO_CHUNK_BYTES];
        memset(chunk, 0xAA, sizeof(chunk));
        picoquic_queue_datagram_frame(cnx, sizeof(chunk), chunk);
        mp_pacer_advance(&g_video_pacer);
    }

    picoquic_set_app_wake_time(cnx, mp_pacer_next_time(&g_video_pacer) < stop_time ?
        mp_pacer_next_time(&g_video_pacer) : stop_time);
}
```

Update the two call sites in `mp_client_callback` (both `picoquic_callback_ready` and `picoquic_callback_app_wakeup` cases) from:
```c
mp_client_send_video_if_due(cnx);
```
to:
```c
mp_client_send_video_if_due(cnx, ((mp_config_t*)callback_ctx)->duration_sec);
```

Apply the same duration-check structure to the server side — replace `mp_server_send_control_if_due`'s body entirely with:

```c
static void mp_server_send_control_if_due(picoquic_cnx_t* cnx, int duration_sec) {
    uint64_t now = picoquic_get_quic_time(picoquic_get_quic_ctx(cnx));

    if (!g_control_pacer_started) {
        mp_pacer_init(&g_control_pacer, now, MP_CONTROL_CHUNK_BYTES, MP_CONTROL_TARGET_BPS);
        g_control_pacer_started = 1;
        picoquic_mark_active_stream(cnx, MP_CONTROL_STREAM_ID, 1, NULL);
    }

    uint64_t stop_time = g_connection_ready_time + (uint64_t)duration_sec * 1000000ULL;
    if (now >= stop_time) {
        fprintf(stderr, "mp_traffic: duration elapsed, closing\n");
        picoquic_close(cnx, 0);
        return;
    }

    while (mp_pacer_is_due(&g_control_pacer, now) && now < stop_time) {
        uint8_t chunk[MP_CONTROL_CHUNK_BYTES];
        memset(chunk, 0xBB, sizeof(chunk));
        picoquic_add_to_stream(cnx, MP_CONTROL_STREAM_ID, chunk, sizeof(chunk), 0);
        mp_pacer_advance(&g_control_pacer);
    }

    picoquic_set_app_wake_time(cnx, mp_pacer_next_time(&g_control_pacer) < stop_time ?
        mp_pacer_next_time(&g_control_pacer) : stop_time);
}
```

And update the two call sites in `mp_server_callback` (both `picoquic_callback_ready` and `picoquic_callback_app_wakeup` cases) from:
```c
mp_server_send_control_if_due(cnx);
```
to:
```c
mp_server_send_control_if_due(cnx, ((mp_config_t*)callback_ctx)->duration_sec);
```
(the server's `picoquic_create` call in Task 4 already passes `config` as the default callback context, so `callback_ctx` here is the same `mp_config_t*` set up there.)

- [ ] **Step 2: Build**

```bash
cd mpquic-test/traffic-app
cmake . && make
```

- [ ] **Step 3: Run a short end-to-end test and confirm clean, automatic shutdown**

**[SERVER]:**
```bash
cd mpquic-test/picoquic
../traffic-app/mp_traffic -p 4433 -G cubic
```

**[CLIENT]:**
```bash
cd mpquic-test/traffic-app
./mp_traffic -A "172.16.2.2/4,172.16.3.2/3" -G cubic --duration 10 10.99.0.1 4433
```

Expected: **no manual Ctrl-C needed this time** — both processes print "duration elapsed, closing" / "connection closed" and exit on their own within a few seconds of the 10-second mark, on both sides (confirms the two-way explicit shutdown, not one side hanging waiting for the other, per the spec's explicit design goal).

- [ ] **Step 4: Verify actual rate via client-side NIC counters (the project's established trusted signal)**

```bash
# before starting the client run:
for ifc in $CLIENT_IFACE_A $CLIENT_IFACE_B $CLIENT_IFACE_C; do cat /sys/class/net/$ifc/statistics/tx_bytes; done
# ... run the 10s test above ...
# after it exits:
for ifc in $CLIENT_IFACE_A $CLIENT_IFACE_B $CLIENT_IFACE_C; do cat /sys/class/net/$ifc/statistics/tx_bytes; done
```

Expected: summed TX byte delta across all 3 interfaces (the client is *sending* the video traffic in this design, so TX not RX) lands in the ballpark of `10 seconds * 10,000,000 bits/sec / 8` ≈ 12.5MB, plus protocol overhead. Note this is TX on the client (video, datagram) — separately check RX on the client too, which should show roughly `10 seconds * 1,000,000 bits/sec / 8` ≈ 1.25MB (the control stream coming back from the server).

- [ ] **Step 5: Commit**

```bash
git add traffic-app/mp_traffic.c
git commit -m "Add duration-based stop condition to mp_traffic"
```

---

### Task 8: Convenience launch scripts (standalone, not sweep-integrated per spec)

**Files:**
- Create: `scripts/run-traffic-server.sh`
- Create: `scripts/run-traffic-client.sh`

**Interfaces:**
- Consumes: `env.sh` (interface names, addresses) — same sourcing pattern as every other script in this repo.

Per the spec's explicit out-of-scope note, this does **not** hook into `run-server-sweep.sh`/`run-client-sweep.sh` — that's a separate future task if wanted. This just makes manual runs as easy as the rest of the repo's tools.

- [ ] **Step 1: Write the server launcher**

```bash
#!/usr/bin/env bash
# Run on SERVER. Usage: ./scripts/run-traffic-server.sh [cc_algo=cubic] [duration_sec=30]
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

cc="${1:-cubic}"
duration="${2:-30}"

cd picoquic
../traffic-app/mp_traffic -p "$QUIC_PORT" -G "$cc"
```

- [ ] **Step 2: Write the client launcher**

```bash
#!/usr/bin/env bash
# Run on CLIENT. Usage: ./scripts/run-traffic-client.sh [cc_algo=cubic] [duration_sec=30]
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

cc="${1:-cubic}"
duration="${2:-30}"

idx() { ip -o link show "$1" | cut -d: -f1 | tr -d ' '; }
IDX_B=$(idx "$CLIENT_IFACE_B")
IDX_C=$(idx "$CLIENT_IFACE_C")

cd traffic-app
./mp_traffic -A "${LINK_B_CLIENT_IP}/${IDX_B},${LINK_C_CLIENT_IP}/${IDX_C}" \
  -G "$cc" --duration "$duration" "$SERVER_CANONICAL_IP" "$QUIC_PORT"
```

- [ ] **Step 3: Make executable and test end-to-end**

```bash
chmod +x scripts/run-traffic-server.sh scripts/run-traffic-client.sh
```

**[SERVER]:** `./scripts/run-traffic-server.sh cubic 15`
**[CLIENT]:** `./scripts/run-traffic-client.sh cubic 15`

Expected: same clean automatic shutdown as Task 7's manual test, now via the repo's standard script convention.

- [ ] **Step 4: Commit**

```bash
git add scripts/run-traffic-server.sh scripts/run-traffic-client.sh
git commit -m "Add convenience launch scripts for mp_traffic"
```
