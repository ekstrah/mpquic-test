#!/usr/bin/env bash
# Alternate to shape-server.sh: uses fq (Fair Queue) with maxrate instead of
# tbf for rate-limiting, to test whether fq's EDT-based packet release
# un-bursts picoquic's UDP GSO batches (picoquic batches paced packets into
# one sendmsg() via UDP_SEGMENT cmsg - see picosocks.c - and a plain tbf's
# small burst bucket may be seeing those batches as overload bursts, which
# would explain BBR's cwnd getting clamped to ~8-10% of correct size while
# CUBIC, being loss-driven only, wasn't thrown off the same way).
#
# CONFIRMED on real hardware: fq is classless, rejects an attached child
# ("Error: Parent qdisc is not classful."). netem does support an attached
# child, so netem is the outer/root qdisc here and fq (holding the rate
# cap) is the inner child - flipped from tbf's root/child order.
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

sudo tc qdisc del dev "$SERVER_IFACE_A" root 2>/dev/null || true
sudo tc qdisc add dev "$SERVER_IFACE_A" root handle 1: netem delay ${LINK_A_DELAY_MS}ms ${LINK_A_JITTER_MS}ms loss ${LINK_A_LOSS_PCT}%
sudo tc qdisc add dev "$SERVER_IFACE_A" parent 1: handle 10: fq maxrate ${LINK_A_RATE_DOWN_MBIT}mbit

sudo tc qdisc del dev "$SERVER_IFACE_B" root 2>/dev/null || true
sudo tc qdisc add dev "$SERVER_IFACE_B" root handle 1: netem delay ${LINK_B_DELAY_MS}ms ${LINK_B_JITTER_MS}ms loss ${LINK_B_LOSS_PCT}%
sudo tc qdisc add dev "$SERVER_IFACE_B" parent 1: handle 10: fq maxrate ${LINK_B_RATE_MBIT}mbit

sudo tc qdisc del dev "$SERVER_IFACE_C" root 2>/dev/null || true
sudo tc qdisc add dev "$SERVER_IFACE_C" root handle 1: netem delay ${LINK_C_DELAY_MS}ms ${LINK_C_JITTER_MS}ms loss ${LINK_C_LOSS_PCT}%
sudo tc qdisc add dev "$SERVER_IFACE_C" parent 1: handle 10: fq maxrate ${LINK_C_RATE_MBIT}mbit

echo "server-side fq+netem shaping applied:"
tc -s qdisc show dev "$SERVER_IFACE_A"
tc -s qdisc show dev "$SERVER_IFACE_B"
tc -s qdisc show dev "$SERVER_IFACE_C"
