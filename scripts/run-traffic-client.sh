#!/usr/bin/env bash
# Run on CLIENT. Usage: ./scripts/run-traffic-client.sh [cc_algo=cubic] [duration_sec=30]
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

cc="${1:-cubic}"
duration="${2:-30}"

idx() { ip -o link show "$1" | cut -d: -f1 | tr -d ' '; }
IDX_B=$(idx "$CLIENT_IFACE_B")
IDX_C=$(idx "$CLIENT_IFACE_C")

cd traffic-app
./mp_traffic -A "${LINK_B_CLIENT_IP}/${IDX_B},${LINK_C_CLIENT_IP}/${IDX_C}" \
  -G "$cc" --duration "$duration" "$SERVER_CANONICAL_IP" "$QUIC_PORT"
