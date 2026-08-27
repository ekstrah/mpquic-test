#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

sudo ip addr replace ${LINK_A_CLIENT_IP}/30 dev "$CLIENT_IFACE_A"
sudo ip link set "$CLIENT_IFACE_A" up
sudo ip addr replace ${LINK_B_CLIENT_IP}/30 dev "$CLIENT_IFACE_B"
sudo ip link set "$CLIENT_IFACE_B" up
sudo ip addr replace ${LINK_C_CLIENT_IP}/30 dev "$CLIENT_IFACE_C"
sudo ip link set "$CLIENT_IFACE_C" up

# Route to the server's canonical address, per source address, so each
# QUIC local-address egresses its matching physical NIC.
sudo ip rule del from ${LINK_A_CLIENT_IP} table 101 2>/dev/null || true
sudo ip rule del from ${LINK_B_CLIENT_IP} table 102 2>/dev/null || true
sudo ip rule del from ${LINK_C_CLIENT_IP} table 103 2>/dev/null || true
sudo ip rule add from ${LINK_A_CLIENT_IP} table 101
sudo ip rule add from ${LINK_B_CLIENT_IP} table 102
sudo ip rule add from ${LINK_C_CLIENT_IP} table 103

sudo ip route replace ${SERVER_CANONICAL_IP}/32 via ${LINK_A_SERVER_IP} dev "$CLIENT_IFACE_A" table 101
sudo ip route replace ${SERVER_CANONICAL_IP}/32 via ${LINK_B_SERVER_IP} dev "$CLIENT_IFACE_B" table 102
sudo ip route replace ${SERVER_CANONICAL_IP}/32 via ${LINK_C_SERVER_IP} dev "$CLIENT_IFACE_C" table 103

# Default (unbound-source) traffic to the server also goes via Link A,
# so a single-path baseline test works with no extra flags.
sudo ip route replace ${SERVER_CANONICAL_IP}/32 via ${LINK_A_SERVER_IP} dev "$CLIENT_IFACE_A"

echo "client addressing + policy routing done:"
ip -brief addr show
ip rule show
ip route show table 101
ip route show table 102
ip route show table 103
