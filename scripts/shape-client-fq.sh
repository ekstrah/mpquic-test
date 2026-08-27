#!/usr/bin/env bash
# Alternate to shape-client.sh - see shape-server-fq.sh for why.
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

sudo tc qdisc del dev "$CLIENT_IFACE_A" root 2>/dev/null || true
sudo tc qdisc add dev "$CLIENT_IFACE_A" root handle 1: fq maxrate ${LINK_A_RATE_UP_MBIT}mbit
sudo tc qdisc add dev "$CLIENT_IFACE_A" parent 1: handle 10: netem delay ${LINK_A_DELAY_MS}ms ${LINK_A_JITTER_MS}ms loss ${LINK_A_LOSS_PCT}%

sudo tc qdisc del dev "$CLIENT_IFACE_B" root 2>/dev/null || true
sudo tc qdisc add dev "$CLIENT_IFACE_B" root handle 1: fq maxrate ${LINK_B_RATE_MBIT}mbit
sudo tc qdisc add dev "$CLIENT_IFACE_B" parent 1: handle 10: netem delay ${LINK_B_DELAY_MS}ms ${LINK_B_JITTER_MS}ms loss ${LINK_B_LOSS_PCT}%

sudo tc qdisc del dev "$CLIENT_IFACE_C" root 2>/dev/null || true
sudo tc qdisc add dev "$CLIENT_IFACE_C" root handle 1: fq maxrate ${LINK_C_RATE_MBIT}mbit
sudo tc qdisc add dev "$CLIENT_IFACE_C" parent 1: handle 10: netem delay ${LINK_C_DELAY_MS}ms ${LINK_C_JITTER_MS}ms loss ${LINK_C_LOSS_PCT}%

echo "client-side fq+netem shaping applied:"
tc -s qdisc show dev "$CLIENT_IFACE_A"
tc -s qdisc show dev "$CLIENT_IFACE_B"
tc -s qdisc show dev "$CLIENT_IFACE_C"
