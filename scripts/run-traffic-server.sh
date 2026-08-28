#!/usr/bin/env bash
# Run on SERVER. Usage: ./scripts/run-traffic-server.sh [cc_algo=cubic] [duration_sec=30]
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

cc="${1:-cubic}"
duration="${2:-30}"

cd picoquic
../traffic-app/mp_traffic -p "$QUIC_PORT" -G "$cc" --duration "$duration"
