#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

for dev_rate in "$SERVER_IFACE_A:$LINK_A_RATE_DOWN_MBIT" "$SERVER_IFACE_B:$LINK_B_RATE_MBIT" "$SERVER_IFACE_C:$LINK_C_RATE_MBIT"; do
  dev="${dev_rate%%:*}"; rate="${dev_rate##*:}"
  sudo tc qdisc del dev "$dev" root 2>/dev/null || true
  sudo tc qdisc add dev "$dev" root handle 1: tbf rate ${rate}mbit burst 32kbit latency 400ms
done

sudo tc qdisc add dev "$SERVER_IFACE_A" parent 1: handle 10: netem delay ${LINK_A_DELAY_MS}ms ${LINK_A_JITTER_MS}ms loss ${LINK_A_LOSS_PCT}%
sudo tc qdisc add dev "$SERVER_IFACE_B" parent 1: handle 10: netem delay ${LINK_B_DELAY_MS}ms ${LINK_B_JITTER_MS}ms loss ${LINK_B_LOSS_PCT}%
sudo tc qdisc add dev "$SERVER_IFACE_C" parent 1: handle 10: netem delay ${LINK_C_DELAY_MS}ms ${LINK_C_JITTER_MS}ms loss ${LINK_C_LOSS_PCT}%

echo "server-side shaping applied:"
tc -s qdisc show dev "$SERVER_IFACE_A"
tc -s qdisc show dev "$SERVER_IFACE_B"
tc -s qdisc show dev "$SERVER_IFACE_C"
