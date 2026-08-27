#!/usr/bin/env bash
# Alternate to shape-server.sh: uses fq (Fair Queue) with maxrate instead of
# tbf for rate-limiting, to test whether fq's EDT-based packet release
# un-bursts picoquic's UDP GSO batches (picoquic batches paced packets into
# one sendmsg() via UDP_SEGMENT cmsg - see picosocks.c - and a plain tbf's
# small burst bucket may be seeing those batches as overload bursts, which
# would explain BBR's cwnd getting clamped to ~8-10% of correct size while
# CUBIC, being loss-driven only, wasn't thrown off the same way).
#
# NOT CONFIRMED: whether fq accepts an attached child qdisc (parent 1:
# handle 10:) the same way tbf does. If the netem attach command below
# errors, that tells us to flip the order (netem root, fq child) instead.
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

for dev_rate in "$SERVER_IFACE_A:$LINK_A_RATE_DOWN_MBIT" "$SERVER_IFACE_B:$LINK_B_RATE_MBIT" "$SERVER_IFACE_C:$LINK_C_RATE_MBIT"; do
  dev="${dev_rate%%:*}"; rate="${dev_rate##*:}"
  sudo tc qdisc del dev "$dev" root 2>/dev/null || true
  sudo tc qdisc add dev "$dev" root handle 1: fq maxrate ${rate}mbit
done

sudo tc qdisc add dev "$SERVER_IFACE_A" parent 1: handle 10: netem delay ${LINK_A_DELAY_MS}ms ${LINK_A_JITTER_MS}ms loss ${LINK_A_LOSS_PCT}%
sudo tc qdisc add dev "$SERVER_IFACE_B" parent 1: handle 10: netem delay ${LINK_B_DELAY_MS}ms ${LINK_B_JITTER_MS}ms loss ${LINK_B_LOSS_PCT}%
sudo tc qdisc add dev "$SERVER_IFACE_C" parent 1: handle 10: netem delay ${LINK_C_DELAY_MS}ms ${LINK_C_JITTER_MS}ms loss ${LINK_C_LOSS_PCT}%

echo "server-side fq+netem shaping applied:"
tc -s qdisc show dev "$SERVER_IFACE_A"
tc -s qdisc show dev "$SERVER_IFACE_B"
tc -s qdisc show dev "$SERVER_IFACE_C"
