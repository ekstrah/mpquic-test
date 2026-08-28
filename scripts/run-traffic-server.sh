#!/usr/bin/env bash
# Run on SERVER. Usage: ./scripts/run-traffic-server.sh [cc_algo=cubic] [duration_sec=30]
# Writes a qlog per run to qlogs_mptraffic/server_<cc>_<timestamp>/ and
# mirrors stdout/stderr to results/mptraffic_server_<cc>_<timestamp>.log,
# matching this repo's existing qlogs/ + results/ sweep-script convention.
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

cc="${1:-cubic}"
duration="${2:-30}"
ts="$(date +%Y%m%d_%H%M%S)"
qlog_dir="qlogs_mptraffic/server_${cc}_${ts}"
log_file="results/mptraffic_server_${cc}_${ts}.log"
# mp_traffic's own --duration timer only starts once a client actually
# connects (armed at picoquic_callback_ready) - if no client ever shows
# up, that timer never starts and the process would otherwise run
# forever. This safety-net timeout (duration + margin, not a guess at
# transfer time) matches run-server-sweep.sh's use of `timeout -s INT`
# for the same reason - SIGINT (not the default SIGTERM) so a stuck
# server still gets picoquic's graceful shutdown path and flushes its
# qlog, same risk noted in that script.
timeout_sec=$((duration + 30))

mkdir -p "$qlog_dir" results

( cd picoquic && exec timeout -s INT "$timeout_sec" \
    ../traffic-app/mp_traffic -p "$QUIC_PORT" -G "$cc" \
    --duration "$duration" -q "../$qlog_dir" ) 2>&1 | tee "$log_file"
