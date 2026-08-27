#!/usr/bin/env bash
# Run on CLIENT. Usage: ./scripts/verify-link.sh {A|B|C}
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

case "$1" in
  A) SRC=$LINK_A_CLIENT_IP; IFACE=$CLIENT_IFACE_A ;;
  B) SRC=$LINK_B_CLIENT_IP; IFACE=$CLIENT_IFACE_B ;;
  C) SRC=$LINK_C_CLIENT_IP; IFACE=$CLIENT_IFACE_C ;;
  *) echo "usage: $0 {A|B|C}"; exit 1 ;;
esac

echo "--- ping (latency/loss) over Link $1 ---"
ping -c 30 -I "$SRC" ${SERVER_CANONICAL_IP}

echo "--- iperf3 (throughput) over Link $1 ---"
echo "requires 'iperf3 -s' already running on SERVER"
iperf3 -c ${SERVER_CANONICAL_IP} -B "$SRC" -t 10
