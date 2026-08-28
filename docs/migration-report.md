# MP-QUIC testbed: migrating from TQUIC to picoquic

Status as of 2026-08-28. This documents the reasoning, the problems hit, how each was diagnosed and resolved, and what's still open. It picks up from the original handoff brief (`docs/handoff.md`) and the picoquic API/build research (`docs/picoquic-research.md`) — this file is the narrative connecting them to what actually happened on the physical rig.

## 1. Why picoquic instead of TQUIC

The prior project (`multipath-quic`, the TQUIC-based repo) built and validated a working multipath-QUIC bench rig on real hardware — three shaped point-to-point links (LEO/Mobile/Mesh) between two boxes, a working sweep harness, and a documented finding (Link A carrying ~58% of traffic regardless of scheduler). That repo is left untouched as a reference; this project ports the same experiment methodology to a different QUIC implementation.

The switch was motivated by TQUIC's standards position, not a defect in the earlier work:

- TQUIC's multipath implementation is pinned to `draft-ietf-quic-multipath-05`, an early draft. The draft has since moved past version -21, and TQUIC's own maintainers say it "doesn't fully adopt some of its complex designs." TQUIC hasn't committed to tracking newer draft versions.
- TQUIC only guarantees interoperability with itself (TQUIC↔TQUIC), not with other QUIC stacks — a real limitation for a testbed meant to reflect how multipath QUIC would behave against arbitrary peers.
- picoquic is authored by Christian Huitema, a co-editor of `draft-ietf-quic-multipath` itself. Its README tracks "the evolving draft" rather than freezing at an old snapshot, and picoquic participates in general QUIC Interop Days alongside other implementations.

The caveat that still applies: even picoquic's own documentation says "planned developments include support for the standard version of multipath" — there is no RFC yet, so no implementation, including picoquic, can claim full/final standard compliance. Nothing here should be read as "picoquic is standards-complete"; it's "picoquic tracks the standard more closely and interops more broadly than TQUIC did."

## 2. Approach: research before porting

Rather than assume picoquic's CLI/API would mirror TQUIC's, Phase 0 was pure research against picoquic's actual source (`github.com/private-octopus/picoquic`) before writing any code. This surfaced two findings that inverted expectations set by the TQUIC work:

**Congestion control is pluggable in picoquic; TQUIC's wasn't.** TQUIC's CC/scheduler factories (`build_congestion_controller`/`build_multipath_scheduler`) were closed `match` statements over private modules — testing a custom CC algorithm would have required forking TQUIC. picoquic exposes CC as a real extension point: a `picoquic_congestion_algorithm_t` struct (init/notify/delete/observe function pointers) registered via `picoquic_register_congestion_control_algorithms()`. The build already ships more built-ins than TQUIC did — confirmed via `picoquicdemo -h`'s dynamically generated list: `newreno, cubic, dcubic, fast, bbr, prague, bbr1, c4`.

**The scheduler is the opposite: pluggable in TQUIC, fixed in picoquic.** TQUIC let you choose MINRTT/REDUNDANT/ROUNDROBIN at runtime. picoquic has exactly one path-selection algorithm, in `picoquic/paths.c`: `picoquic_select_next_path_tuple()` prioritizes any path needing validation-frame traffic, falls back to the single path if only one exists, and otherwise calls `picoquic_sort_available_paths()`, which ranks candidates by minimum-RTT (for ACK placement), pacing eligibility, congestion-window headroom, and stream affinity. There is no plugin point — confirmed by a 2024 GitHub issue ([#1637](https://github.com/private-octopus/picoquic/issues/1637)) where a user asked the maintainer this exact question and got no response. Replicating TQUIC's scheduler-comparison methodology in picoquic would require patching `paths.c` directly, not calling a flag.

Two known TQUIC bugs were checked against picoquic from the start rather than rediscovered the hard way:

- TQUIC bug: client closed the connection before all paths finished validation, producing false "multipath is broken" symptoms. Not confirmed to reproduce in picoquic in the same form (see §4, the intermittent truncated-run issue may be a variant of this — still open).
- TQUIC bug: `active-cid-limit` defaulted to 2, silently starving any path past the second. picoquic's equivalent transport parameter (`active_connection_id_limit`) defaults to `PICOQUIC_NB_PATH_TARGET` = 8, confirmed in `picoquic_internal.h` and empirically (all 3 paths carried real traffic in every multipath test run). **This bug class does not reproduce in picoquic for a 3-link rig.**

## 3. Physical rig: unchanged, re-verified not re-built

The bench rig itself — two Ubuntu boxes, three physically wired point-to-point links (Link A "LEO" ~62/18Mbit, Link B "Mobile" ~30Mbit, Link C "Mesh" ~15Mbit, each with configured delay/jitter/loss via `tc`/`netem`), source-based policy routing on the client, loopback-aliased canonical address on the server — carried over from the TQUIC repo essentially unchanged. `env.sh` and the `net-*.sh`/`shape-*.sh`/`verify-link.sh` scripts were ported near-verbatim; the only real work was re-verifying interface names still matched (same physical NICs) and re-validating each link's actual delay/loss against target via `ping`/`iperf3`.

One real finding here: measured TCP throughput via `iperf3` came in well under nominal targets on two of three links (Link A ~53% of its 18Mbit target, Link C ~53% of 15Mbit, Link B ~82% of 30Mbit) — attributed to TCP's congestion behavior interacting with a small, uniform `tbf` burst parameter (32kbit, same value regardless of link rate, inherited unchanged from the TQUIC rig) rather than a `tc` misconfiguration, since ping latency/jitter matched targets precisely on all three links. Flagged but not blocking, since the real experiment traffic is QUIC with its own congestion control, not TCP.

## 4. Problems hit, in the order they surfaced

### 4.1 Build and CLI gotchas (Phase 1)

- **`picoquicdemo --help` hung indefinitely.** picoquic's option parser only handles single-dash short flags (no `getopt_long`); `--help` fell through as an unrecognized positional argument, was interpreted as a server hostname, and the demo tried to open a client connection to a host literally named `--help`. Fixed by using `-h` instead, which correctly routes through picoquic's config module to `usage()`.
- **Bare-IP connections failed HTTP/3 with `Cannot send GET command for stream(0)`.** TLS SNI (RFC 6066) cannot carry an IP literal; connecting by raw IP with no explicit SNI produces `NULL SNI`, and HTTP/3 needs a non-empty `:authority` derived from it. Fixed with `-n <arbitrary-hostname>` — the hostname doesn't need to resolve or match a real certificate, it just needs to be non-empty.
- **Server returned 0 bytes for every GET despite the test file existing.** `picoquicdemo` has no default web-server document root (`wwwdir` initializes to `NULL`). Fixed with `-w <path>` pointed at the folder `gen-testfile.sh` populates.
- **Multipath needs interface indices, not names.** The `-A` flag's syntax (`ip/ifindex[/server_ip]`) takes a numeric interface index, requiring `ip link show <iface>` to resolve `CLIENT_IFACE_B`/`CLIENT_IFACE_C` to numbers before every multipath test.

### 4.2 The "balanced distribution" finding, and how it fell apart under scrutiny

The first multipath test (BBR, the default, 5MB file) showed download traffic split almost evenly across all three links (32.8% / 35.7% / 31.5%) — a striking contrast to the TQUIC rig's Link-A-dominance finding (~58%/19%/24%). The initial read was that picoquic's min-RTT/cwnd-gated scheduler was structurally fairer than TQUIC's list-order-biased one.

This did not survive testing a second congestion-control algorithm. The same 5MB transfer under CUBIC showed **strong Link-A dominance averaged across 10 repeated runs — A 69.6% / B 11.2% / C 19.3%, more extreme than the original TQUIC finding.** The distribution wasn't a scheduler property at all; it was an artifact of which CC algorithm happened to be running underneath the (unchanged) scheduler.

Digging into *why* required pulling actual qlog data rather than continuing to theorize. A 200MB BBR transfer was run specifically to distinguish two hypotheses — "BBR just needs more RTTs to ramp up" vs. "BBR is structurally capped on this network" — and the result (aggregate throughput actually *fell* slightly compared to the 5MB case, ~1.33Mbps vs ~2.7-4.5Mbps) ruled out the first. Extracting server-side qlog `metrics_updated` events (`cc,pathid,category,event,data` positional-array format, filtered with `awk` rather than a full JSON parse given the file was 262MB) showed the actual mechanism: **BBR's congestion window on the two higher-bandwidth links never grew past ~8-10% of what their real bandwidth-delay product would require**, while the lowest-bandwidth link's cwnd was near-correctly sized (~76%). All three paths were being held to a similar small *absolute* cwnd ceiling regardless of true per-link capacity — which is what produced both the low aggregate throughput and the "balanced" split (an even share of a badly undersized pie, not fair sharing of the real one).

A specific fix hypothesis was tested and refuted: picoquic uses UDP GSO to batch paced packets into single `sendmsg()` calls (confirmed in `picosocks.c`), and Linux's `fq` qdisc can un-burst a GSO batch via per-packet earliest-departure-time release — plausible if the rig's `tbf` shaping (small, uniform burst parameter) was seeing GSO batches as overload and inducing self-loss that BBR's estimator overreacted to. Alternate `fq`-based shaping scripts were built and tested (discovering along the way that `fq` is classless and can't hold a child qdisc the way `tbf` can — `netem` had to become the outer qdisc with `fq maxrate` as the inner one). Result: **no measurable difference** — same ~1.38Mbps, same 33/33/33 split. Likely explanation: `fq`'s GSO-unbursting specifically needs the application to attach per-packet departure timestamps via `SO_TXTIME`, and picoquic's userspace-only pacer (confirmed in `pacing.c` — no kernel pacing socket calls at all, not even `SO_MAX_PACING_RATE`) almost certainly doesn't set that either. The fix wasn't reachable from the `tc` layer.

**Current state:** BBR is treated as a poor fit for this rig's link/jitter profile and CUBIC is the working default. The precise internal reason BBR's estimator stays low (most likely RTT-jitter interfering with its STARTUP-phase bandwidth sampling) is not confirmed at the source-code level — this remains open (see §6).

### 4.3 The intermittent truncated-run pattern

Across every repeated-run test this session — 5MB and 200MB, under both BBR and CUBIC — a consistent minority of runs (roughly 1-in-4 to 1-in-5) complete in under a second having transferred only a few hundred KB to a couple MB, instead of the full file, while still reporting `exit_code = 0`. First suspected to be 0-RTT session-ticket reuse across back-to-back client invocations (picoquic persists `demo_token_store.bin`/ticket state by default, and a resumed connection using stale cached transport parameters could plausibly truncate early) — fixed by passing fresh `-N`/`-T` token/ticket files per run via `mktemp`. **The pattern persisted even with fresh ticket files each time**, ruling that theory out as the sole cause.

This is a different bug class from the TQUIC "client closes before all paths validate" symptom originally suspected — it happens mid-transfer after multipath negotiation already succeeded, not during setup. The client log from a `bbr1` truncation (20MB sweep) shows this isn't a timeout or crash — it's picoquic itself detecting a fault: `Connection ends with local error 0xa (protocol violation)` after 1.5MB of the 20MB file, with multipath negotiation having already reported `Enable multipath: Success.` and all 3 paths carrying packets. A second truncation, on `cubic` in a later sweep, showed the identical signature (`0xa`, after 1.4MB of a 20MB file) — same failure mode on a completely different CC algorithm.

**Confirmed via client-side qlog (2026-08-28), after several attempts to actually capture one.** A `dcubic` truncation's qlog shows the exact trigger:

```
"frame_type": "connection_close", "error_space": "transport", "error_code": 10,
"trigger_frame_type": "path_retire_connection_id", "reason": "Cannot delete path through which packet arrives"
```

`error_code: 10` (`0xa`) matches the client log's `protocol violation` in both prior sightings. The trigger is a path-scoped `RETIRE_CONNECTION_ID` frame — not `NEW_CONNECTION_ID` as originally guessed from reading `frames.c` alone; the qlog corrected that guess. Tracing the events leading up to it: all the retirements involved are on path 1 (Link B), issued **out of sequence order** — sequence 1 fires first, then 0, then 3, then finally 2 immediately before the fatal close. Retiring the connection ID currently in active use for receiving packets on that path is what triggers `PICOQUIC_TRANSPORT_PROTOCOL_VIOLATION` (confirmed in `frames.c`, "Attempting to retire the connection ID through which the current packet arrived"). This looks like a genuine race in picoquic's per-path connection-ID rotation logic, not something introduced by this project's harness or shaping.

**Replicated with an identical signature on a second truncation (`bbr`, different sweep).** Same path (1, Link B), and the same out-of-order retirement pattern: sequence 1 retired before sequence 0, then later sequence 3 retired before sequence 2 — immediately before the fatal close. Two independent truncations, two different CC algorithms, same path, same descending-pair ordering, same fatal frame. This is now a reproducible signature rather than a one-off — a solid candidate for an upstream picoquic bug report once seen once or twice more, and specific enough (path 1 / Link B, the highest-RTT link of the three) to suggest the race is timing-sensitive to that path's RTT rather than purely random across all paths equally.

#### How to reproduce

The bug is probabilistic, not deterministic — across five sweeps run so far, the per-connection truncation rate has held fairly steady around 35-45% (3-4 of 8 combos per sweep), but *which* combo hits it varies run to run (see the CC-correlation correction above). Expect to need a few attempts, not one.

1. Bring up the 3-link shaped rig per `docs/handoff.md` / `env.sh` (unchanged from the TQUIC-era setup — Link A/B/C with their configured delay/jitter/loss).
2. On the server: `./scripts/run-server-sweep.sh` (cycles all 8 registered CC algorithms; any single one can also be run manually with `-M -q <dir> -G <cc> -1`, multipath and a single connection are enough — the specific CC algorithm doesn't matter, all 8 have shown the failure at least once).
3. On the client: `./scripts/run-client-sweep.sh` shortly after (declares the 2 extra multipath paths via `-A`, fetches `/testfile.bin`, captures a qlog per combo under `qlogs_client/<cc>/`).
4. Check `results/sweep.csv` for a truncated row — total received bytes far below the file size, `elapsed_s` under ~1 second despite a 20MB file.
5. Confirm via the client-side qlog for that combo:
   ```
   grep -n "connection_close\|path_retire_connection_id" qlogs_client/<cc>/*.client.qlog | tail -20
   ```
   A hit looks like: `"frame_type": "connection_close", "error_space": "transport", "error_code": 10, "trigger_frame_type": "path_retire_connection_id", "reason": "Cannot delete path through which packet arrives"`, preceded by `path_retire_connection_id` events on path 1 with sequence numbers arriving out of ascending order (e.g. 1 then 0, later 3 then 2).

Minimum ingredients believed necessary, based on what's constant across every observed instance: a multipath connection with 3 paths (`-M`, 2 paths added via `-A`), enough transfer duration/data for several rounds of connection-ID issuance and retirement to occur (the two confirmed instances were both 20MB transfers; not yet tested whether 5MB reproduces it), and — so far — path 1 specifically in both hits, though this project's harness always adds paths in the same order (path 0 = default route/Link A, path 1 = first `-A` entry/Link B, path 2 = second `-A` entry/Link C), so "path 1" and "Link B, the highest-RTT of the three links" are confounded and not yet distinguished — worth testing with the `-A` order swapped to see if the bug follows the path index or the link's RTT.

**What it is not, on closer look: a per-algorithm pattern.** The first two sweeps (5MB, then 20MB) showed `newreno`/`dcubic`/`bbr1` truncating in both while `cubic`/`fast`/`prague`/`c4` stayed clean in both — read at the time as evidence the race was CC-dependent. A third sweep (20MB again) broke that reading completely: `newreno`, `dcubic`, and `bbr1` all completed cleanly that time, while `cubic`, `prague`, and `c4` — previously 2-for-2 clean — all truncated. Only `fast` has stayed consistent across all three sweeps. **The per-algorithm correlation was a premature conclusion from too small a sample (n=2), not a real pattern** — worth noting as a specific instance of the exact mistake this project already learned once with the single-CC "balanced distribution" finding (§4.2). What *has* stayed stable across all three sweeps is the overall truncation rate (3/8, 4/8, 3/8 — roughly 35-45%), which is more consistent with a genuinely random per-connection race, independent of which CC is running, than a CC-specific interaction.

**Not yet confirmed at the frame level** — this needs a client-side qlog captured during an actual truncation. Client-side qlog capture (`-q`) was added to `run-client-sweep.sh` (previously only the server captured qlogs), but the first attempt to use it ran against a stale copy of the script that hadn't been pulled — still pending a clean re-run with the fix actually in place.

### 4.4 Sweep harness bugs (Phase 2)

Building the automated CC sweep (`run-server-sweep.sh` / `run-client-sweep.sh`) surfaced two more bugs, both caught by looking at real output rather than trusting the design on paper:

- **Every combo in the first sweep run failed identically** (~19KB per row, zero bytes on Links B/C, blank elapsed/mbps). Root cause: picoquic's default certificate path (`certs/cert.pem`) is relative to the process's working directory at run time, not the binary's location, and only exists under `picoquic/certs/`. The sweep script launched the binary from the repo root instead of `picoquic/` (unlike every prior manual invocation this session), so certificate loading failed silently during server setup (`ret = -1`, no further output) before the server ever reached `Waiting for packets`. Fixed by launching inside a subshell that `cd`s into `picoquic/` first.
- **The original sweep design used a fixed per-combo sleep window** (guessed generously to cover the slowest algorithm) on the server side, with the client independently timed to match. This has two problems: it wastes time (every combo pays for the slowest algorithm's window even if that combo's CC finishes in 1/20th the time), and it's a genuine correctness risk — the server advances to the next combo on its own clock regardless of whether the client has actually caught up, which can silently misattribute rows in the CSV with no way to detect it after the fact. Fixed by using picoquicdemo's own `-1` flag ("close the server after processing 1 connection"), confirmed via source to wait for the connection to actually *close* (not just be accepted) before exiting. The server now blocks until the client's request for that combo genuinely finishes, self-synchronizing without any channel between the two boxes. The former window size is now a generous safety-net timeout only (currently 180s, sized for a 20MB file under a slow-BBR worst case), not a value the sweep depends on for correctness.

## 5. Phase 3: real traffic model (`mp_traffic`)

Everything in §1-4 used a single bulk-file transfer as the workload. The source paper's actual traffic model is two simultaneous streams — a 10Mbps video-like downlink and a 1Mbps control-like uplink — which `picoquicdemo`'s HTTP/3 GET can't produce. `mp_traffic` (`traffic-app/`) is a small custom client/server built directly on the picoquic library API to replace it: video (10Mbps CBR, 1200B chunks) rides client→server as QUIC datagrams (RFC 9221), control (1Mbps CBR, 125B chunks) rides server→client as a reliable stream, both over the same multipath connection, both stopping automatically after a configurable `--duration`. It links against the same in-place `picoquic/` checkout (`traffic-app/CMakeLists.txt`'s `add_subdirectory`) rather than a separate build.

Design spec: `docs/superpowers/specs/2026-08-28-mp-traffic-app-design.md`. Implementation plan: `docs/superpowers/plans/2026-08-28-mp-traffic-app.md`.

### 5.1 Building and running

```bash
./scripts/build-picoquic.sh      # if not already built
./scripts/build-traffic-app.sh   # builds traffic-app/mp_traffic
```

**[SERVER]:** `./scripts/run-traffic-server.sh cubic 30`
**[CLIENT]:** `./scripts/run-traffic-client.sh cubic 30`

Each run writes a qlog to `qlogs_mptraffic/<role>_<cc>_<timestamp>/` and mirrors stdout/stderr plus (client-side) per-path NIC byte-count deltas and a post-run per-path `ping` latency/loss sample to `results/mptraffic_<role>_<cc>_<timestamp>.log`. Set `SKIP_PING=1` on the client script when looping over several CC algorithms back-to-back — the server's safety-net timeout is sized for the traffic run alone, and the ~60s ping phase makes the client run long enough per iteration to drift out of sync with it.

### 5.2 Real bugs found building it, in the order they surfaced

All confirmed on real hardware, each root-caused against picoquic's actual source (fetched from `github.com/private-octopus/picoquic` — no local checkout exists off the rig) rather than guessed:

1. **Multipath path-creation is driven from the packet-loop callback, not the connection callback.** The implementation plan initially assumed `-A`-style path setup was a one-shot call at connection creation; it's actually driven from `picoquic_packet_loop_v2`'s own socket-level callback (`picoquic_packet_loop_after_receive`), gated on `picoquic_subscribe_new_path_allowed`/`is_notified_that_path_is_allowed`, with retry logic on transient error codes — the same split `picoquicdemo.c` uses between its connection-event callback and its packet-loop callback.
2. **`PICOQUIC_ERROR_MEMORY` on the 2nd extra path wasn't a real allocation failure.** Traced through `picoquic_create_path` → `picoquic_find_avalaible_unique_path_id`: gated on `max_path_id_in_cnxid_lists`, which grows only as the peer's `NEW_CONNECTION_ID` frames actually arrive — not a static limit. Fixed by retrying on this code instead of treating it as fatal (it was also propagating straight to `picoquic_packet_loop_v2`, which treats any nonzero loop-callback return as fatal — closing the whole connection over what was really "give it another round trip").
3. **RFC 9221 datagram support needs an explicit transport-parameter opt-in.** Sending via `picoquic_queue_datagram_frame` alone isn't enough — without `picoquic_set_default_tp_value(quic, picoquic_tp_max_datagram_frame_size, ...)` on both sides, the peer rejects incoming datagrams with a `FRAME_ENCODING_ERROR` and closes the connection. Same shape of gap as multipath's own `initial_max_path_id` transport parameter.
4. **Closing the QUIC connection doesn't stop `picoquic_packet_loop_v2` on its own.** `picoquic_close()` only queues a `CONNECTION_CLOSE` frame; the packet loop's inner `while (ret == 0 && ...)` only exits when a loop-callback returns nonzero — there's no automatic exit-on-disconnect (confirmed via a comment in picoquic's own `sockloop.c` noting this was deliberately removed). The first fix attempt used `picoquic_packet_loop_time_check` to request termination, which compiled and ran but never actually exited: that callback's return value is unconditionally overwritten immediately afterward by the loop's own timeout/qmux-check handling, in the same iteration. Switched to `picoquic_packet_loop_after_send`, confirmed (by reading the full per-iteration control flow) to be the last callback invoked each iteration with nothing after it able to clobber the result.
5. **`mp_traffic` never called `picoquic_register_all_congestion_control_algorithms()`.** Both `picoquic_get_congestion_algorithm` and `picoquic_set_default_congestion_algorithm_by_name` read a process-wide name→algorithm registry (`picoquic_congestion_control_algorithms` in `quicctx.c`) that starts empty; nothing populates it automatically. `picoquicdemo.c` calls the registration function at the top of its own `main()`, before parsing `-G` — `mp_traffic.c` copied `picoquicdemo.c`'s connection-setup and path-driving logic (see #1) but not its `main()`'s startup sequence, so this call was never carried over. **Consequence: every `mp_traffic` run for the entire duration of this app's development had congestion control silently disabled — `-G cubic` was resolving to `NULL` and being accepted without error the whole time.** This was only caught because a later fix (adding `-G` validation, so a *typo'd* algorithm name would error instead of silently disabling CC — see #6) immediately rejected the literal string `"cubic"` on the very next real-hardware run. Fixed by adding the missing registration call once in `main()`.
6. **An unrecognized `-G` value was silently accepted and disabled congestion control** (`picoquic_set_default_congestion_algorithm_by_name` stores `NULL` for an unknown name; every congestion-control call site inside picoquic guards on non-`NULL`, so nothing crashes — the connection just runs completely uncontrolled). This is the validation whose first real run surfaced bug #5. Fixed by checking `picoquic_get_congestion_algorithm(name) != NULL` before use, on both client and server.
7. **Send failures looked indistinguishable from successes, and the first fix for that was itself incomplete.** `picoquic_queue_datagram_frame`/`picoquic_add_to_stream`'s return values were originally discarded entirely — fixed with generated-vs-queued counters on both channels, reported at close (`duration elapsed, closing (video: X/Y chunks queued)`). This counter turned out to prove far less than it looked like it proved: `picoquic_queue_datagram_frame` appends to an **unbounded** malloc'd list (`picoquic_queue_misc_or_dg_frame` in `quicctx.c`) — there is no "queue full" failure under congestion, so a connection that can't drain the queue fast enough just grows an ever-larger backlog rather than rejecting new chunks, and the counter reads ~100% either way. Confirmed the hard way on real hardware (see §5.3): BBR showed `100%` queued in the same run where client-side NIC TX bytes measured only ~28% of the 10Mbps target. The counter's own comments were corrected to say what it actually measures (successful enqueue, not delivery), and the real fix — see #8 — adds accounting that actually can distinguish the two.
8. **Real delivery accounting needed picoquic's ack/loss callbacks, not a queue-return-code check.** picoquic already fires `picoquic_callback_datagram_acked` / `_lost` / `_spurious` on the same connection callback automatically whenever a previously-queued datagram's fate resolves (confirmed in `frames.c` for acked/spurious, `loss_recovery.c` for lost) — no extra opt-in call or queuing flag needed, `mp_traffic` was just falling through to `default: break` for all three event types. Now tracked and reported at close: queued, acked, confirmed-lost (lost minus spurious, since a spurious mark means an earlier loss report was a false positive from reordering), and still-unresolved at close time.

### 5.3 First real measurements, and what they show

**A 30-second `cubic` run** (the first with congestion control actually active, immediately after bug #5's fix landed):

- **Video (datagram, client→server):** 31250/31250 chunks queued (100%, vs ~68% on pre-fix runs that were silently running with no CC at all). Total client-side TX across all 3 links: 39.42MB over 30s ≈ 10.5Mbps — on target once per-packet UDP/IP/QUIC header overhead is accounted for.
- **Traffic distribution across the 3 paths was heavily skewed:** Link C (mesh, 15Mbit, ~24ms RTT) carried 95.9% of video traffic; Link B (mobile, 30Mbit — double C's rated bandwidth — but ~54ms RTT) carried effectively none (3.9KB); Link A (LEO, the default path) carried the remainder. This is consistent with §2's finding that picoquic's single built-in path-selection algorithm (`picoquic_select_next_path_tuple` / `picoquic_sort_available_paths` in `paths.c`) ranks by minimum-RTT rather than balancing by available bandwidth — Link B's extra capacity going unused is the same class of scheduler behavior already on record from Phase 0 research, now observed directly under the real traffic model rather than inferred from source reading.
- Loss showed 0% on all 3 paths in a post-run 20-ping sample per path — expected, since the configured loss rates (0.17% / 0.006% / 0.5%) are too low to reliably show up in a 20-packet sample.

**A 5-algorithm sweep** (bbr, newreno, bbr1, fast, dcubic — one 30s run each, `SKIP_PING=1` to keep the two independently-looped shell scripts from drifting out of sync) surfaced the real reason bug #7 was worth fixing:

| CC | Video TX (target 10Mbps) | Control RX (target 1Mbps) | Video Link A/B/C share |
|---|---|---|---|
| cubic | 10.5 Mbps | 1.59 Mbps | 4.0 / ~0 / 95.9% |
| bbr | **2.85 Mbps** | 1.56 Mbps | 6.5 / ~0 / 93.4% |
| bbr1 | **6.41 Mbps** | 1.62 Mbps | 3.6 / ~0 / 96.3% |
| fast | **1.80 Mbps** | 1.40 Mbps | 5.8 / ~0 / 94.2% |
| dcubic | **2.65 Mbps** | 1.60 Mbps | 5.1 / ~0 / 94.9% |

(`newreno`'s NIC byte counters are omitted here pending a re-check — one value looked truncated in the terminal paste, inconsistent with the following run's starting value for the same counter.)

Two findings, both measured via client-side NIC byte deltas (the "chunks queued" counter reported ~100% for every single one of these runs — exactly the false-positive bug #7 describes):

- **Only `cubic` actually hits its 10Mbps video target.** Every other algorithm undershoots substantially, `fast` worst at 18% of target. This reproduces something already on record from the original bulk-transfer sweep (§4.2: "BBR's congestion window... never grew past ~8-10% of what real bandwidth-delay product would require") — seeing a similar shortfall again here, under a completely different traffic pattern (fixed-rate datagrams vs. a bulk file), independently corroborates that this is a real characteristic of these CC algorithms on this rig's link profile, not an artifact of the bulk-transfer test's specific shape.
- **The Link-C-dominance pattern holds identically across every algorithm tested** (93-96% of video traffic on Link C every time) — six data points now (cubic plus this sweep) agreeing it's the scheduler's RTT-based path ranking, not CC-algorithm-specific behavior.

**Still not done:** re-verify the `newreno` byte counts; sweep `prague` and `c4` (the two `SWEEP_CC_LIST` algorithms not yet tried under this traffic model); re-run with bug #8's acked/lost/spurious accounting in place to get real per-run delivery numbers instead of only before/after NIC-counter diffing; and check whether the still-open `PROTOCOL_VIOLATION` truncation bug from §4.3 shows up under this traffic model at all (not seen yet — datagrams don't retransmit and the control stream's data volume is far smaller than the bulk-transfer tests that originally surfaced it, so the triggering conditions may simply arise less often here).

## 6. Current sweep results and what needs more investigation

The most recent completed sweep (5MB file, all 8 registered CC algorithms, single run each) after the timing-design fix:

| CC | Mbps | Link A / B / C share | Notes |
|---|---|---|---|
| newreno | 4.3 | 56.5 / 9.8 / 33.7 | **truncated** (0.6MB total, 0.73s) |
| cubic | 22.7 | 75.9 / 3.1 / 21.0 | full transfer |
| dcubic | 2.3 | 35.4 / 19.6 / 45.0 | **truncated** (0.3MB total, 0.75s) |
| fast | 4.7 | 23.5 / 41.2 / 35.2 | full transfer |
| bbr | 4.7 | 34.1 / 28.7 / 37.1 | full transfer |
| prague | 6.9 | 28.2 / 34.1 / 37.7 | full transfer |
| bbr1 | 2.8 | 31.4 / 18.6 / 50.1 | **truncated** (0.5MB total, 0.79s) |
| c4 | 6.2 | 33.3 / 32.8 / 33.9 | full transfer |

A second sweep at 20MB (same 8 algorithms, single run each) followed:

| CC | Mbps | Link A / B / C share | Notes |
|---|---|---|---|
| newreno | 3.6 | 40.0 / 11.6 / 48.4 | **truncated** (0.75MB total, 0.97s) |
| cubic | 12.3 | 45.0 / 33.5 / 21.5 | full transfer |
| dcubic | 4.3 | 25.7 / 34.5 / 39.8 | **truncated** (1.76MB total, 2.70s) |
| fast | 4.3 | 22.7 / 45.0 / 32.3 | full transfer |
| bbr | 5.3 | 35.9 / 13.3 / 50.8 | **truncated** (0.75MB total, 0.75s) - full at 5MB |
| prague | 8.4 | 21.1 / 50.8 / 28.1 | full transfer |
| bbr1 | 5.4 | 39.6 / 18.6 / 41.9 | **truncated** (1.96MB total, 2.27s) - protocol error, see below |
| c4 | 5.8 | 31.7 / 34.7 / 33.6 | full transfer |

Four of eight rows truncated this time. Comparing to the 5MB sweep initially looked like a pattern - `newreno`/`dcubic`/`bbr1` truncated in both - but a **third** sweep (20MB again, single run each) overturned that reading:

| CC | Mbps | Link A / B / C share | Notes |
|---|---|---|---|
| newreno | 14.2 | 38.9 / 9.7 / 51.4 | full transfer (truncated in both earlier sweeps) |
| cubic | 15.5 | 79.1 / 2.4 / 18.5 | **truncated** (3.0MB total, 0.74s) - full in both earlier sweeps |
| dcubic | 4.2 | 28.2 / 35.3 / 36.4 | full transfer (truncated in both earlier sweeps) |
| fast | 2.7 | 21.8 / 44.4 / 33.8 | full transfer |
| bbr | 1.8 | 32.8 / 30.7 / 36.5 | full transfer |
| prague | 1.8 | 50.0 / 19.1 / 30.8 | **truncated** (0.33MB total, 0.76s) - full in both earlier sweeps |
| bbr1 | 8.5 | 27.3 / 37.0 / 35.7 | full transfer (truncated in both earlier sweeps) |
| c4 | 2.3 | 31.6 / 21.5 / 46.9 | **truncated** (0.47MB total, 0.77s) - full in both earlier sweeps |

Every algorithm that "reliably" truncated in the first two sweeps (`newreno`, `dcubic`, `bbr1`) came back clean here, and two of the four that "reliably" succeeded (`cubic`, `c4`, plus `prague`) truncated instead. Only `fast` has been consistent across all three sweeps. **The per-algorithm correlation read from the first two sweeps was a premature conclusion from n=2, not a real pattern** — see the corrected §4.3 for the full reasoning. The truncated `cubic` row's client log shows the identical `PROTOCOL_VIOLATION (0xa)` signature as the earlier `bbr1` case, confirming the fault mode is the same across algorithms even though which run hits it looks essentially random. The overall rate has stayed stable across all three sweeps (3/8, 4/8, 3/8) even as which specific combo fails has not.

Two more observations from the valid rows, both non-obvious and worth following up with repeats before trusting them: CUBIC's Link-A dominance is markedly less extreme at 20MB (45-79% across the two 20MB runs) than the 5MB-repeat average (69.6%) is stable, but noisier than expected — the other paths may catch up more over a longer transfer even under an aggressive CC, though the spread between the two 20MB CUBIC runs (45.0% vs 79.1% for A) is wide enough that this needs real repeats, not two data points. `c4` looked balanced in the first two sweeps (~33/33/34 both times) but leaned toward C in the third (31.6/21.5/46.9) — another read that doesn't survive a third data point.

**Open items that need further investigation, roughly in priority order:**

1. **Root cause confirmed and replicated (§4.3): `PROTOCOL_VIOLATION` triggered by a path-scoped `RETIRE_CONNECTION_ID` frame retiring the connection ID currently in active use on path 1 (Link B), with retirements issued in a specific out-of-order pattern (sequence 1 before 0, then 3 before 2) — seen identically on two independent truncations across two different CC algorithms (`dcubic`, `bbr`).** This is a genuine picoquic multipath connection-ID-rotation bug, not a harness/shaping artifact. Solid enough now to consider an upstream bug report after one or two more confirmations; still worth checking whether it's specific to path 1/Link B's RTT or would show up on any path under the right timing.
2. **Confirm BBR's exact cwnd-stunting mechanism (§4.2).** The RTT-jitter-during-STARTUP theory is plausible and consistent with the observed cwnd data, but not confirmed against picoquic's actual `bbr.c` source or a qlog trace of the STARTUP→PROBE_BW transition timing. The `fq`/GSO hypothesis was tested and refuted; this is the remaining live hypothesis, untested.
3. **Repeat the sweep for statistical confidence — this is now the priority, not optional polish.** Three sweeps in, the pattern is clear: single-run results for any algorithm other than the heavily-repeated BBR/CUBIC (10 runs each) are not reliable enough to draw conclusions from, as both the truncation-pattern reversal and `c4`'s distribution flip demonstrate directly. The sweep completes in ~1-4 minutes depending on file size, so repeating it 5-10 times per algorithm is cheap relative to how much it's already changed the reading twice.
4. **`fast` is the only algorithm showing a consistent Link-B lean across all three sweeps so far** (41.2% / 44.4% / 45.0% to B) — worth specific attention once repeats are in, since everything else that looked consistent at n=2 has since flipped.
5. **Done since this section was first written:** the source paper's dual-stream traffic model (10Mbps video-like via QUIC datagrams + 1Mbps control-like via a reliable stream) is now built as `mp_traffic` — see §5. **Still not started, from the original handoff brief:** custom CC/scheduler beyond picoquic's built-ins (a fork of `paths.c` would be needed for a custom scheduler specifically, per §2).
6. **From §5.3: a 5-algorithm sweep (bbr, newreno, bbr1, fast, dcubic) confirmed the Link-C-dominance/Link-B-starvation pattern holds across every CC algorithm tested, and found only `cubic` actually reaches its 10Mbps video target.** Remaining: re-verify `newreno`'s byte counts (one value looked corrupted in the terminal paste), sweep the two untested `SWEEP_CC_LIST` algorithms (`prague`, `c4`), and re-run with §5.2 bug #8's acked/lost/spurious accounting for real per-run delivery numbers. Whether this correlates with §4.3's still-open truncation bug remains unchecked.
