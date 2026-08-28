# Design: custom bidirectional traffic-model app (`mp_traffic`)

Status: approved by user 2026-08-28, ready for implementation planning.

## Purpose

Replace the single-bulk-file transfer used everywhere so far with the source paper's actual traffic model — a continuous video-like stream competing with a continuous control-like stream over the same multipath connection — so the sweep results reflect the real scenario this testbed is meant to study, not a proxy for it.

## Context this design must comply with

- **Multipath: `draft-ietf-quic-multipath-20`**, confirmed via `picoquic_internal.h`'s frame-type constant comments (`/* Per quic multipath draft 20 */`). Still a draft, not an RFC — this app inherits that caveat, same as every other picoquic multipath test in this project.
- **Datagrams: RFC 9221**, already finalized — a different maturity level than multipath, worth stating explicitly wherever this app's traffic model is described so the two aren't conflated.
- **Deployment framing** (from `docs/handoff.md`): server = ground station, single link; client = aerial vehicle, three uplinks. "Downlink"/"uplink" are from the vehicle's perspective, not the generic client/server one — the vehicle (client) pushes video *down* to the ground station (server) using its multipath advantage; the ground station sends control commands *up* to the vehicle on a single link.

## Architecture

New standalone directory, `traffic-app/`, sibling to `picoquic/` and `scripts/` — **not** inside the `picoquic/` checkout, so that checkout stays a clean, pullable clone of upstream.

- `mp_traffic.c` — forked from `picoquicfirst/picoquicdemo.c`'s connection-setup path (multipath negotiation, `-A` path declaration, cert/key loading, the event loop) rather than built from picoquic's `sample/` app, because the multipath wiring in `picoquicdemo.c` is already proven working across this entire project, and re-deriving that wiring from scratch in a from-scratch app is exactly the kind of code most likely to introduce a new bug — on top of the one already found in picoquic's own multipath implementation this session (the `RETIRE_CONNECTION_ID` race, see `docs/migration-report.md` §4.3). Everything unrelated to connection setup — the HTTP/3 GET-scenario parser, the multi-resource-fetch machinery — gets stripped out; this app has exactly one behavior per role, not a configurable scenario language.
- `CMakeLists.txt` — links against the already-built `picoquic/` library in place (no vendoring, no re-fetching).

### CLI surface

Mirrors `picoquicdemo` where the semantics are identical, so muscle memory from the rest of this project carries over:

- Server: `./mp_traffic -p <port> -G <cc>`
- Client: `./mp_traffic -A <path-spec> -G <cc> --duration <seconds> <server_ip> <port>`
- `-A` uses the identical `ip/ifindex[/server_ip],...` syntax already validated all session.
- `-G` reuses the same CC algorithm list as the sweep harness (`newreno, cubic, dcubic, fast, bbr, prague, bbr1, c4`).
- `--duration <seconds>` is the one new flag — no equivalent exists in `picoquicdemo` since it only ever does one-shot fetches.
- Multipath (`-M`) is implicit and always on — this app has no non-multipath mode, unlike `picoquicdemo`.

## Data flow

| Role | Traffic | Mechanism | Target rate | Direction |
|---|---|---|---|---|
| Client (vehicle) | video-like | QUIC datagram (RFC 9221) | ~10Mbps | Client → Server, across all 3 paths |
| Server (ground station) | control-like | QUIC stream | ~1Mbps | Server → Client, single link |

Rationale for datagrams on the video-like side: loss-tolerant, no head-of-line blocking holding up stale frames — the standard reason real video-over-QUIC systems prefer datagrams over streams, and the reason this project flagged datagram support as a goal separately from the traffic-model work before folding the two together. Rationale for a stream on the control side: control commands need reliability and ordering, which is exactly what a stream provides and a datagram doesn't.

Both directions are **constant-bitrate**: fixed-size chunks sent at a fixed interval computed from `(chunk_size, target_rate)`, not variable/bursty. This is the simple, verifiable baseline — a real video encoder's actual bitrate variability is out of scope for this pass.

Payload content is filler bytes on both sides. Only the traffic *pattern* (rate, size, direction, timing) matters for studying multipath scheduling behavior; payload semantics don't.

**Per-link attribution is not done inside the app.** This project's established, validated methodology — client-side NIC RX/TX byte counters captured before/after a run (`/sys/class/net/*/statistics/{rx,tx}_bytes`), the same approach `run-client-sweep.sh` already uses — continues to be the source of truth for which physical link carried how much traffic. The app doesn't need to instrument per-path byte counts itself; that would duplicate a mechanism already proven trustworthy and add a second, unvalidated way to get the same number.

## Pacing mechanism

picoquic has no built-in rate limiting or continuous-send support (confirmed by reading `picoquicdemo.c`/`config.c` directly — no bandwidth flag, no repeat/loop scenario syntax exists). Pacing is therefore application-level:

1. On connection-ready, compute `interval_us = chunk_size_bytes * 8 * 1_000_000 / target_bps`.
2. Track `next_send_time` per traffic-generating role.
3. Hook into picoquic's existing event-loop wake-time mechanism (the same `next_wake_time` pattern `picoquicdemo.c` already uses for its own internal timers) to request a callback at `next_send_time` rather than polling or busy-waiting — this fits the library's existing loop structure instead of fighting it.
4. On each wake, if `now >= next_send_time`: send one chunk (datagram via `picoquic_mark_datagram_ready_path`/the `picoquic_callback_prepare_datagram` callback on the client; stream write on the server), advance `next_send_time 
+= interval_us`.

## Stop condition

- Wall-clock start time recorded at connection-ready (not at process start, so handshake time doesn't eat into the requested duration).
- Once `duration` seconds have elapsed, the traffic-generating side on each end stops scheduling new sends.
- Each side then calls `picoquic_close()` explicitly — a deliberate two-way shutdown, not reliance on an implicit "peer stopped sending, must be done" signal, which would be fragile to prove correct (and this project has already spent considerable effort chasing a subtle premature-close-style bug in picoquic's own multipath code — this app should not add a second, home-grown one).

## Testing approach

No mocked unit-test framework. The correctness question that actually matters — does this behave right against the real shaped links, under real jitter/loss — can only be answered by the real rig, the same way every other piece of this project has been validated:

- Does the client-side NIC RX/TX byte-counter delta over a run land near the target rate (10Mbps aggregate on the video path, 1Mbps on the control path)?
- Does the qlog show clean periodic sends with no unexpected `connection_close` (specifically watching for a recurrence of the `RETIRE_CONNECTION_ID` bug already found — if this app hits it too, that's further confirmation it's a picoquic-level issue, not specific to `picoquicdemo`'s scenario mechanics)?
- Does the connection close cleanly at the requested duration, on both sides, without one side hanging waiting for the other?

## Explicitly out of scope for this pass

- Variable-bitrate traffic shaping (confirmed CBR-only in brainstorming).
- Per-path in-app metrics/instrumentation (external NIC-counter methodology covers this already).
- Integration into the automated sweep harness (`run-server-sweep.sh`/`run-client-sweep.sh`) — this app is validated standalone first; sweep integration is a natural follow-up once it works, not part of this design.
