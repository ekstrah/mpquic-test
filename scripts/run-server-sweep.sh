#!/usr/bin/env bash
# Run on SERVER. Cycles through every CC algorithm in SWEEP_CC_LIST,
# restarting picoquicdemo fresh for each - no live channel to the client,
# both sides just loop the same list in the same order (see
# run-client-sweep.sh). Uses -1 ("close the server after processing 1
# connection") instead of a fixed sleep window: picoquicdemo then blocks
# until that one connection actually closes (confirmed via source - it
# waits for picoquic_get_first_cnx() to go NULL, not just "accepted")
# before exiting on its own. That means the server naturally waits for
# whatever the client actually needs, rather than progressing to the next
# combo on an independent clock the way a fixed sleep would - which was
# both wasteful (every combo paying for the slowest algorithm's window)
# and risky (a fixed window that quietly starts before the client catches
# up desyncs every later row in the CSV with no way to detect it).
# SWEEP_WINDOW_SEC is now a safety-net timeout, not a guess at how long a
# transfer takes - only kicks in if a connection never completes at all.
# timeout -s INT (not the default SIGTERM) so a stuck server still gets
# picoquic's graceful shutdown path if the safety net does fire - SIGTERM
# killed TQUIC's sweep before qlog flushed on clean close, same risk here.
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

mkdir -p results qlogs
for cc in "${SWEEP_CC_LIST[@]}"; do
  echo "=== CC=$cc (waiting for the connection to complete, capped at ${SWEEP_WINDOW_SEC}s) ==="
  mkdir -p "qlogs/$cc"
  # picoquicdemo's default cert/key path (certs/cert.pem) is relative to
  # its OWN working directory at run time, not to the binary's location -
  # it only exists under picoquic/certs/, so the binary must be launched
  # with picoquic/ as cwd (matching every manual invocation this session)
  # or it fails silently during setup (ret=-1, no further output).
  ( cd picoquic && exec timeout -s INT "${SWEEP_WINDOW_SEC}" \
      ./picoquicdemo -w ../www -p "$QUIC_PORT" -M -q "../qlogs/$cc" -G "$cc" -1 ) \
    > "results/server_${cc}.log" 2>&1 || true
done

echo "sweep done (server side)."
