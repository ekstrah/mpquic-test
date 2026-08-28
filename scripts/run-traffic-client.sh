#!/usr/bin/env bash
# Run on CLIENT. Usage: ./scripts/run-traffic-client.sh [cc_algo=cubic] [duration_sec=30]
# Writes a qlog per run to qlogs_mptraffic/client_<cc>_<timestamp>/ and
# mirrors stdout/stderr to results/mptraffic_client_<cc>_<timestamp>.log,
# matching this repo's existing qlogs/ + results/ sweep-script convention.
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

cc="${1:-cubic}"
duration="${2:-30}"
ts="$(date +%Y%m%d_%H%M%S)"
qlog_dir="qlogs_mptraffic/client_${cc}_${ts}"
log_file="results/mptraffic_client_${cc}_${ts}.log"

idx() { ip -o link show "$1" | cut -d: -f1 | tr -d ' '; }
IDX_B=$(idx "$CLIENT_IFACE_B")
IDX_C=$(idx "$CLIENT_IFACE_C")

mkdir -p "$qlog_dir" results

( cd traffic-app && exec ./mp_traffic -A "${LINK_B_CLIENT_IP}/${IDX_B},${LINK_C_CLIENT_IP}/${IDX_C}" \
    -G "$cc" --duration "$duration" -q "../$qlog_dir" "$SERVER_CANONICAL_IP" "$QUIC_PORT" ) \
  2>&1 | tee "$log_file"
