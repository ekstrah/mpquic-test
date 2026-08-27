#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh
for dev in "$SERVER_IFACE_A" "$SERVER_IFACE_B" "$SERVER_IFACE_C"; do
  sudo tc qdisc del dev "$dev" root 2>/dev/null || true
done
echo "run the analogous 'sudo tc qdisc del dev <iface> root' on the CLIENT box too."
