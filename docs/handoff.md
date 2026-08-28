# Project brief: MP-QUIC ground testbed, picoquic port

Paste this whole document as the first message in a new Claude Code project/repo. It's a handoff from a prior project (`multipath-quic`, TQUIC-based) that built and validated a working multipath-QUIC bench rig. The goal now is to rebuild the same experiment harness against **picoquic** instead of TQUIC, for reasons explained below. Read this fully before writing any code — several sections describe hardware constraints and past bugs that are easy to reintroduce if skipped.

## Research context

Replicating the methodology of "Multipath Transport Analysis Over Cellular and LEO Access for Aerial Vehicles" (Baltaci et al., IEEE Access 2023), but with Multipath QUIC instead of MPTCP/MP-DCCP. The paper's own testbed used MoonGen (LTE emulation) + OpenSAND (LEO emulation) + netem on a single workstation with virtual interfaces; this project uses real hardware instead.

Target deployment (real, future): server = ground station with a single outbound link; client = aerial vehicle with three uplinks (LEO satellite, Mobile/cellular, Wireless Mesh).

## Why picoquic instead of TQUIC

The prior project used TQUIC (Tencent, Rust). Investigation found:
- TQUIC's multipath implementation is pinned to `draft-ietf-quic-multipath-05` (an early draft) and the maintainer explicitly says it "doesn't fully adopt some of its complex designs." The draft has since moved to version -21+, and TQUIC hasn't committed to tracking it.
- TQUIC only guarantees interoperability with itself (TQUIC↔TQUIC), not with other QUIC stacks.
- picoquic is authored by Christian Huitema, a co-editor of `draft-ietf-quic-multipath` itself, and its README tracks "the evolving draft" rather than being frozen at an old snapshot. picoquic also participates in the general QUIC Interop Days alongside other implementations.
- **Caveat that still applies**: even picoquic's own README says "planned developments include support for the standard version of multipath" — there is no RFC yet, no implementation can claim full/final standard compliance. Whatever you build, disclose which draft version/commit picoquic tracks at build time, and note whether multipath interop was actually tested cross-implementation or only picoquic↔picoquic.

**First task in the new project**: research picoquic's actual current multipath CLI/API (repo: `github.com/private-octopus/picoquic`). Do not assume it mirrors TQUIC's flags below — picoquic's build system (CMake + OpenSSL/picotls, not Cargo), CLI surface, and scheduler/CC extension points are almost certainly different and may have changed since this brief was written. Confirm current reality before porting.

## Physical rig (unchanged — this hardware setup carries over as-is)

- Two Linux mini PCs in a lab, **not reachable over SSH from the dev machine**. Distro is Ubuntu/Debian (apt).
- Each mini PC has 3 physical NICs. Three independent point-to-point links are wired between them:
  - **Link A ("LEO")**: ~62 Mbit down / ~18 Mbit up, ~25ms delay ±13ms jitter, ~0.17% loss.
  - **Link B ("Mobile")**: ~30 Mbit symmetric (static approximation), ~50ms delay ±5ms jitter, ~0.006% loss. Time-varying handover bursts NOT modeled.
  - **Link C ("Mesh")**: ~15 Mbit symmetric, ~20ms delay ±8ms jitter, ~0.5% loss — placeholder estimate, not derived from real mesh-radio measurements. Replace with real numbers once a mesh radio is characterized.
- The server is single-homed at the QUIC-app level (one listen address); the bench rig gives it 3 NICs purely so each client path can be shaped independently without contention.
- **Routing consequence**: because the client must reach the *same* server destination address via three different NICs/next-hops over disjoint L3 segments, the server's canonical address goes on a **loopback alias** (not tied to any physical NIC), and the client uses **source-based policy routing** (`ip rule` / per-table `ip route`) so traffic sourced from each local link address goes out the matching NIC. `rp_filter` must be relaxed on the server's physical NICs (strict reverse-path filtering would otherwise drop inbound packets whose destination doesn't match the receiving interface's subnet).
- Interface names are hardware-specific and unknown ahead of time — discover them with `ip -brief link show` on each box and record in an `env.sh` that every script sources, same pattern as before.

## Collaboration constraints (read before doing anything)

- **No SSH access from the dev machine to either lab box.** The user runs every command by hand on the physical hardware and pastes results back. Every script/instruction must be copy-pasteable as-is, clearly labeled `[SERVER]` or `[CLIENT]`.
- Don't paste full DEBUG-level logs back and forth by default — ask for targeted `grep`/`awk` output first, escalate to full logs only if that doesn't resolve it.
- When debugging, walk through evidence layer by layer (routing → shaping → physical NIC → app) and ask for one command's output at a time.
- **Prefers step-by-step execution over big-bang implementation.** Propose a step breakdown first, implement one step at a time, check in between. Confirm before assuming an earlier decision still holds. git commit/push are separate confirmations from "build this."

## What the TQUIC version built (blueprint to replicate, not code to port verbatim)

Phase 1 (hardware/network proof):
1. Wire and shape the 3 point-to-point links to their target profiles (tc/netem/tbf), verify with a bandwidth/latency/loss test tool.
2. Get the QUIC library building on both boxes.
3. Single-path QUIC connection works (baseline sanity check).
4. Multipath QUIC connection comes up using all links simultaneously, confirmed via qlog showing multiple active paths and per-interface packet counters (`ip -s link show`) showing traffic on all NICs.

Experiment harness (built after Phase 1, in incremental steps):
1. Test-file generator (5MB file so CC/scheduler sweeps have real data to move).
2. Client/server launch scripts taking scheduler algorithm + congestion-control algorithm as positional args.
3. Server-side and client-side **sweep scripts** that loop over every (scheduler × CC) combo in lockstep, with no live channel between the two boxes to resync (fixed time window per combo: server restarts, client waits N seconds then benchmarks for M seconds). Writes a `results/sweep.csv` with per-run stats: connection success, requests sent/finished/succeeded, req/s, latency median/p99, recv/sent/lost bytes, lost packets, goodput bytes, and **per-link RX/TX packet-counter deltas** (`ip -s link show dev $ifc`, parsed before/after each run) — this is the metric that actually reflects real traffic distribution across links, see finding below.
4. A self-contained (no CDN dependency) HTML results viewer: paste/upload the sweep.csv, it charts goodput, latency, wire-overhead, and per-link packet share.

## Real bugs hit and fixed (check for equivalents in picoquic)

1. **Client closed the connection before all paths finished validation.** The client app was exiting/closing as soon as its primary request completed, racing ahead of the other 1-2 paths still completing path validation — so multipath verification looked broken when it was actually a timing race. Fix was on the TQUIC client's shutdown logic; picoquic will need the equivalent "wait for all paths validated" handling, whatever that looks like in its API.
2. **`--active-cid-limit` defaulted to 2**, silently starving every path past the 2nd of a usable connection ID (multipath needs one CID per path; with a 3-link rig this capped usable paths at 2 and the 3rd link carried no traffic, with no error — just silent starvation). Check whatever picoquic's equivalent connection-ID-limit parameter is and make sure it's set to at least the number of links, from the start.

## Link-A-dominance finding (methodology lesson, re-derive the specifics for picoquic — don't assume they transfer)

Original observation on the TQUIC rig: Link A (highest bandwidth) carried far more RX traffic than B/C on every scheduler, including round-robin and redundant — confirmed via `sweep.csv`'s per-link RX packet-count columns (Link A: 57.8% share vs ~19-24% for B/C in one representative run).

Root cause in TQUIC: all built-in schedulers funnel through one shared `on_select()` call per outgoing packet that returns the *first* path with congestion-window headroom, and round-robin falls back to rescanning from the front of the path list (index 0 = first-added link) whenever later paths aren't cwnd-ready — a structural bias toward whichever link is added first, compounding with that link's cwnd naturally refilling faster.

**This is TQUIC-implementation-specific** (specific scheduler source, specific factory pattern) — don't assume picoquic's scheduler(s) have the same bias without checking its actual scheduling source. But the **debugging methodology is transferable and worth reusing**:
- Don't trust a congestion-control trace log (e.g. per-path `cwnd`/send-count from a short foreground debug run) as a proxy for "which link actually carried more traffic" — it measures a different thing (server-side per-path send dynamics over a short window) than the real signal.
- The real signal is **client-side NIC packet counters** (`ip -s link show dev $ifc`, RX/TX deltas) captured across the *full* benchmark window, already being logged into `sweep.csv` if you replicate that harness step. Check that column first before reaching for lower-level protocol tracing.

## Not-yet-done items to carry over as goals for the new project

- Statistical confidence: the original sweep was one run per (scheduler × CC) combo; repeat for averages/variance once the picoquic harness is working.
- Custom (non-built-in) congestion-control algorithms and/or schedulers were out of scope for TQUIC without forking it (`build_congestion_controller`/`build_multipath_scheduler` were closed factories over private modules with no plugin API). **Check picoquic's extension points for this before assuming a fork is needed** — it may expose CC/scheduler as a pluggable interface already, which would remove an entire phase of work.
- Handover-burst modeling on the Mobile link (latency/loss spikes) — explicitly deferred in the original spec, still not modeled.
- ~~Replicating the source paper's actual traffic model (10 Mbps downlink video-like stream + 1 Mbps uplink control-like stream) instead of a single bulk-file transfer — was out of scope for Phase 1.~~ **Done** — see `docs/migration-report.md` §5 (`mp_traffic`, `traffic-app/`).

## Suggested first steps in the new project

1. Research picoquic's current multipath CLI/API and build requirements (don't assume TQUIC parity).
2. Re-run the physical-rig network setup (link wiring, shaping scripts, `env.sh` interface discovery) — this part is protocol-agnostic and should port with minimal changes.
3. Get picoquic building on both boxes, single-path connection working first (baseline sanity check), then multipath across all 3 links.
4. Re-check for the two TQUIC bugs' equivalents (path-validation-vs-close race, CID/path limit defaults) before trusting any multipath result.
5. Rebuild the sweep harness once multipath is confirmed working, prioritizing the client-side NIC-packet-counter methodology over log-based proxies from the start.
