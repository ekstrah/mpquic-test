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

**Current state:** BBR is treated as a poor fit for this rig's link/jitter profile and CUBIC is the working default. The precise internal reason BBR's estimator stays low (most likely RTT-jitter interfering with its STARTUP-phase bandwidth sampling) is not confirmed at the source-code level — this remains open (see §5).

### 4.3 The intermittent truncated-run pattern

Across every repeated-run test this session — 5MB and 200MB, under both BBR and CUBIC — a consistent minority of runs (roughly 1-in-4 to 1-in-5) complete in under a second having transferred only a few hundred KB to a couple MB, instead of the full file, while still reporting `exit_code = 0`. First suspected to be 0-RTT session-ticket reuse across back-to-back client invocations (picoquic persists `demo_token_store.bin`/ticket state by default, and a resumed connection using stale cached transport parameters could plausibly truncate early) — fixed by passing fresh `-N`/`-T` token/ticket files per run via `mktemp`. **The pattern persisted even with fresh ticket files each time**, ruling that theory out as the sole cause.

This is a different bug class from the TQUIC "client closes before all paths validate" symptom originally suspected — it happens mid-transfer after multipath negotiation already succeeded, not during setup. A sweep run (20MB file, all 8 CC algorithms) surfaced a real lead: comparing truncation across two independent sweeps (5MB and 20MB), three algorithms - `newreno`, `dcubic`, `bbr1` - truncated in **both** runs, while `cubic`, `fast`, `prague`, `c4` completed cleanly in **both**. That's not what a purely random per-connection race would produce; it points at something correlated with which CC algorithm is running.

The client log from a `bbr1` truncation (20MB sweep) confirms this isn't a timeout or crash - it's picoquic itself detecting a fault: `Connection ends with local error 0xa (protocol violation)` after 1.5MB of the 20MB file, with multipath negotiation having already reported `Enable multipath: Success.` and all 3 paths carrying packets. Checking `picoquic/frames.c` for where `PICOQUIC_TRANSPORT_PROTOCOL_VIOLATION` gets raised turned up a specific, plausible mechanism: a path-scoped `NEW_CONNECTION_ID` frame referencing a path ID the peer isn't yet authorized to use (`path_id` exceeding `max_path_id_local`) - and picoquic's own source comment on this check notes "out-of-order arrival might cause the peer's max_path_id logic to behave unexpectedly," i.e. a plausible race in issuing connection IDs for the 2nd/3rd path that could easily be timing/pacing-sensitive, which would explain the clustering on the more conservative, slower-ramping algorithms. **Not confirmed** - this requires a client-side qlog to verify against the actual frame sequence, which isn't currently captured (only the server has `-q`; see §5 item 1).

### 4.4 Sweep harness bugs (Phase 2)

Building the automated CC sweep (`run-server-sweep.sh` / `run-client-sweep.sh`) surfaced two more bugs, both caught by looking at real output rather than trusting the design on paper:

- **Every combo in the first sweep run failed identically** (~19KB per row, zero bytes on Links B/C, blank elapsed/mbps). Root cause: picoquic's default certificate path (`certs/cert.pem`) is relative to the process's working directory at run time, not the binary's location, and only exists under `picoquic/certs/`. The sweep script launched the binary from the repo root instead of `picoquic/` (unlike every prior manual invocation this session), so certificate loading failed silently during server setup (`ret = -1`, no further output) before the server ever reached `Waiting for packets`. Fixed by launching inside a subshell that `cd`s into `picoquic/` first.
- **The original sweep design used a fixed per-combo sleep window** (guessed generously to cover the slowest algorithm) on the server side, with the client independently timed to match. This has two problems: it wastes time (every combo pays for the slowest algorithm's window even if that combo's CC finishes in 1/20th the time), and it's a genuine correctness risk — the server advances to the next combo on its own clock regardless of whether the client has actually caught up, which can silently misattribute rows in the CSV with no way to detect it after the fact. Fixed by using picoquicdemo's own `-1` flag ("close the server after processing 1 connection"), confirmed via source to wait for the connection to actually *close* (not just be accepted) before exiting. The server now blocks until the client's request for that combo genuinely finishes, self-synchronizing without any channel between the two boxes. The former window size is now a generous safety-net timeout only (currently 180s, sized for a 20MB file under a slow-BBR worst case), not a value the sweep depends on for correctness.

## 5. Current sweep results and what needs more investigation

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

Four of eight rows truncated this time, and comparing the two sweeps directly is the important part: **`newreno`, `dcubic`, and `bbr1` truncated in both runs; `cubic`, `fast`, `prague`, `c4` completed cleanly in both.** Only `bbr` is inconsistent (full at 5MB, truncated at 20MB). That pattern - not scattered randomly across all 8, but repeating on the same three algorithms - is the basis for the sharpened root-cause lead in §4.3 (a likely multipath connection-ID/path-ID race, evidenced by an actual `PROTOCOL_VIOLATION (0xa)` in the `bbr1` client log, not a timeout or crash).

Two more observations from the valid rows, both non-obvious and worth following up with repeats before trusting them: CUBIC's Link-A dominance is markedly less extreme at 20MB (45.0/33.5/21.5) than the 5MB-repeat average (69.6/11.2/19.3) - the other paths may catch up more over a longer transfer even under an aggressive CC. And `fast` (B-leaning both sizes: 41→45%) and `c4` (balanced both sizes: ~33/33/34 both times) show the same shape at both file sizes, suggesting a real per-algorithm property rather than single-run noise - though every result here is still n=1 or n=2, not statistically confident.

**Open items that need further investigation, roughly in priority order:**

1. **Confirm the multipath path-ID race hypothesis for the truncated-run pattern (§4.3).** The candidate mechanism (a `NEW_CONNECTION_ID` frame referencing an unauthorized path ID, per `picoquic/frames.c`) is plausible and matches the CC-clustering evidence, but unconfirmed - needs a client-side qlog (add `-q` to the client's picoquicdemo invocation, not currently captured) from a `newreno`/`dcubic`/`bbr1` truncation to verify against the actual frame sequence.
2. **Confirm BBR's exact cwnd-stunting mechanism (§4.2).** The RTT-jitter-during-STARTUP theory is plausible and consistent with the observed cwnd data, but not confirmed against picoquic's actual `bbr.c` source or a qlog trace of the STARTUP→PROBE_BW transition timing. The `fq`/GSO hypothesis was tested and refuted; this is the remaining live hypothesis, untested.
3. **Repeat the sweep for statistical confidence.** Every result so far is n=1 or n=10 for exactly two algorithms (BBR, CUBIC) — the other six have never been repeated. Given the sweep now completes in ~75 seconds, repeating it several times is cheap and should happen before drawing conclusions about any algorithm other than BBR/CUBIC.
4. **Investigate `fast`'s Link-B-favoring split** (41.2% to B, the only non-BBR/CUBIC result that doesn't cluster near-even) — could be a real property of that algorithm or could be within single-run noise; needs repeats to tell.
5. **Not yet started, from the original handoff brief:** custom CC/scheduler beyond picoquic's built-ins (a fork of `paths.c` would be needed for a custom scheduler specifically, per §2), the source paper's actual dual-stream traffic model (10Mbps video-like + 1Mbps control-like) in place of a single bulk transfer, and QUIC datagram-based transport for the video-like stream (RFC 9221 — supported by picoquic's library API and already integrated with its multipath scheduler, but not exposed by `picoquicdemo`; would need a small custom app, most easily scaffolded from picoquic's own `sample/` client/server rather than written from scratch).
