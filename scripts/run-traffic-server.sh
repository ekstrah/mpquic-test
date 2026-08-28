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

mkdir -p "$qlog_dir" results

( cd picoquic && exec ../traffic-app/mp_traffic -p "$QUIC_PORT" -G "$cc" \
    --duration "$duration" -q "../$qlog_dir" ) 2>&1 | tee "$log_file"
