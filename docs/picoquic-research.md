# picoquic multipath/CC/build research (Phase 0)

Sourced from `github.com/private-octopus/picoquic` `master` branch, 2026-08-27. Re-check against the actual checked-out commit once we clone — draft tracking and defaults can move.

## 1. Multipath support

- CLI: demo app (`picoquicfirst/picoquicdemo.c`) has `-M` to enable the multipath extension, and `-A "client_ip/ifindex[/server_ip],..."` on the **client** to declare each additional path's local IP + interface index (+ optional distinct server IP). Example from usage text: `-A "10.0.0.2/3,10.0.0.3/4/10.1.1.1"`. For same-IP-different-port testing: `-A ::0/0`.
- API (`picoquic/picoquic.h`): `picoquic_set_default_multipath_option()`, `picoquic_probe_new_path_ex()` (start a path, takes interface index), `picoquic_abandon_path()`, `picoquic_set_path_status()` (mark a path `available` or `backup`), `picoquic_get_path_quality()` (RTT/cwnd/etc for app-level scheduling decisions), `picoquic_set_stream_path_affinity()` (pin a stream to one path).
- Draft version: README only says it tracks "the evolving draft," no pinned version number in docs. **Still need to check the actual source comments/CHANGELOG at the commit we build from** to satisfy the brief's disclosure requirement (which draft version, picoquic↔picoquic only vs cross-implementation tested).

## 2. Build

- Deps: Picotls + OpenSSL (1.1.1 or 3.0). Picotls needs a specific tested commit per picoquic's docs — **check the exact commit hash in the README/CMakeLists at clone time**, don't assume it's still current.
- `cmake -DPICOQUIC_FETCH_PTLS=Y . && make` builds picotls automatically; otherwise build/install picotls separately first.
- CMake, not Cargo — build scripts from the TQUIC repo need rewriting, not porting.

## 3. Congestion control — pluggable, richer than TQUIC (no fork needed)

```c
typedef struct st_picoquic_congestion_algorithm_t {
    char const * congestion_algorithm_id;
    uint8_t congestion_algorithm_number;
    uint8_t ecn_mark;
    picoquic_congestion_algorithm_init alg_init;
    picoquic_congestion_algorithm_notify alg_notify;
    picoquic_congestion_algorithm_delete alg_delete;
    picoquic_congestion_algorithm_observe alg_observe;
} picoquic_congestion_algorithm_t;
```
Registered via `picoquic_register_congestion_control_algorithms()`, selected via `picoquic_set_default_congestion_algorithm()` or per-connection `picoquic_set_congestion_algorithm_ex()`. Demo CLI: `-G <name>` (default `bbr`).

Built-ins already in the source tree: `bbr`, `bbr1`, `cubic`, `newreno`, `prague`, `fastcc`, `c4`, `scone`, `dualq_aqm` — more than TQUIC shipped. **A genuinely novel algorithm still needs writing, but hooking it in is a normal struct + registration call, not a fork** — this closes out the brief's open question (§ "custom CC... out of scope without forking TQUIC"). No fork needed for CC.

## 4. Scheduler — NOT pluggable, opposite of CC (fork/patch likely needed)

No factory or plugin point exists for path *selection* (as opposed to congestion control). One built-in algorithm, in `picoquic/paths.c`:

`picoquic_select_next_path_tuple()` → decision order:
1. Any path needing to send a challenge/response (path-validation) frame gets picked immediately.
2. If only one path exists/is usable, use it.
3. Otherwise `picoquic_sort_available_paths()` ranks candidates by: min-RTT priority (for acks), pacing eligibility, congestion-window headroom (`bytes_in_transit < cwin`), stream-affinity pins, then datagram-readiness.

This is a single min-RTT/cwnd-based heuristic, not swappable schedulers like TQUIC's round-robin/redundant/minrtt. Confirmed there's no undocumented plugin surface either — [issue #1637](https://github.com/private-octopus/picoquic/issues/1637) asked the maintainer this exact question in 2024 and got no response; closed unanswered. **To replicate TQUIC's round-robin/redundant scheduler comparison, we'd need to patch `paths.c` directly** — this is the one place a fork is probably unavoidable, inverse of the CC situation.

## 5. Connection-ID / path-limit default — likely already safe

`active_connection_id_limit` transport parameter defaults to `PICOQUIC_NB_PATH_TARGET` = **8** (`picoquic_internal.h`), set in `picoquic_init_transport_parameters()`. No CLI flag for it in `config.c` — `-I` only sets CID *length* (default 8 bytes), not the count limit. Override, if ever needed, is `picoquic_set_default_tp_value(quic, picoquic_tp_active_connection_id_limit, N)`.

**This means TQUIC bug #2 (CID limit defaulting to 2, starving path 3) likely does not reproduce out of the box** for our 3-link rig — default of 8 covers it. Still verify empirically once we have a real 3-path connection up (Phase 1, step 6/7).

## 6. Still open / unresolved from this research pass

- TQUIC bug #1 equivalent (client closing before all paths validate) — did not find explicit "wait for all paths validated" logic in `picoquicdemo.c`'s client shutdown path in this pass. Needs checking directly in the demo client's completion/close code once cloned, or via `picoquic_get_path_quality()` polling before close.
- Exact draft version + interop scope disclosure (picoquic↔picoquic only vs cross-implementation) — not found in README excerpt fetched, check `doc/` folder and any CHANGELOG at clone time.
