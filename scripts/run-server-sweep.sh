#!/usr/bin/env bash
# Run on SERVER. Cycles through every CC algorithm in SWEEP_CC_LIST,
# restarting picoquicdemo fresh for each with a fixed time window - no
# live channel to the client, both sides just loop the same list in the
# same order (see run-client-sweep.sh). Uses SIGINT (not the default
# SIGTERM) to stop each server instance - TQUIC's sweep harness lost this
# fight once already: SIGTERM killed the process before qlog flushed on
# clean connection close, leaving empty qlog files. SIGINT triggers
# picoquic's graceful shutdown path instead.
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

mkdir -p results qlogs
for cc in "${SWEEP_CC_LIST[@]}"; do
  echo "=== CC=$cc, window=${SWEEP_WINDOW_SEC}s ==="
  mkdir -p "qlogs/$cc"
  # picoquicdemo's default cert/key path (certs/cert.pem) is relative to
  # its OWN working directory at run time, not to the binary's location -
  # it only exists under picoquic/certs/, so the binary must be launched
  # with picoquic/ as cwd (matching every manual invocation this session)
  # or it fails silently during setup (ret=-1, no further output).
  # `exec` inside the subshell replaces it with picoquicdemo itself, so
  # $! below is picoquicdemo's real PID, not a wrapper shell's.
  ( cd picoquic && exec ./picoquicdemo -w ../www -p "$QUIC_PORT" -M -q "../qlogs/$cc" -G "$cc" ) \
    > "results/server_${cc}.log" 2>&1 &
  server_pid=$!
  sleep "$SWEEP_WINDOW_SEC"
  kill -INT "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
done

echo "sweep done (server side)."
