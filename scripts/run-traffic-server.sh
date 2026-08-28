#!/usr/bin/env bash
# Run on SERVER. Usage: ./scripts/run-traffic-server.sh [cc_algo=cubic] [duration_sec=30]
# Writes a qlog per run to qlogs_mptraffic/server_<cc>_<timestamp>/ and
# mirrors stdout/stderr to results/mptraffic_server_<cc>_<timestamp>.log,
# matching this repo's existing qlogs/ + results/ sweep-script convention.
# Set SKIP_QLOG=1 to omit qlog output - qlog size scales with duration
# (a 30s run produces ~30-37MB; an hour-long run would be ~120x that,
# multiple GB per run), so long runs where you only need the delivery
# counters/NIC byte deltas should skip it.
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

cc="${1:-cubic}"
duration="${2:-30}"
ts="$(date +%Y%m%d_%H%M%S)"
qlog_dir="qlogs_mptraffic/server_${cc}_${ts}"
log_file="results/mptraffic_server_${cc}_${ts}.log"
qlog_args=()
if [ "${SKIP_QLOG:-0}" != "1" ]; then
  qlog_args=(-q "../$qlog_dir")
fi
# mp_traffic's own --duration timer only starts once a client actually
# connects (armed at picoquic_callback_ready) - if no client ever shows
# up, that timer never starts and the process would otherwise run
# forever. This safety-net timeout (duration + margin, not a guess at
# transfer time) matches run-server-sweep.sh's use of `timeout -s INT` -
# mp_traffic (unlike picoquicdemo) installs its own SIGINT handler that
# routes through the same clean-shutdown path as a normal duration
# close, so a fired safety net still flushes its qlog rather than being
# silently killed.
timeout_sec=$((duration + 30))

mkdir -p results
if [ "${SKIP_QLOG:-0}" != "1" ]; then
  mkdir -p "$qlog_dir"
fi

# `|| true` so a fired safety net (exit 124) doesn't kill this script -
# and, when this script is itself looped over several CC algorithms,
# doesn't take the rest of that loop down with it.
( cd picoquic && exec timeout -s INT "$timeout_sec" \
    ../traffic-app/mp_traffic -p "$QUIC_PORT" -G "$cc" \
    --duration "$duration" "${qlog_args[@]}" ) 2>&1 | tee "$log_file" || true
