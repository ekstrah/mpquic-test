#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

sudo ip addr replace ${LINK_A_SERVER_IP}/30 dev "$SERVER_IFACE_A"
sudo ip link set "$SERVER_IFACE_A" up
sudo ip addr replace ${LINK_B_SERVER_IP}/30 dev "$SERVER_IFACE_B"
sudo ip link set "$SERVER_IFACE_B" up
sudo ip addr replace ${LINK_C_SERVER_IP}/30 dev "$SERVER_IFACE_C"
sudo ip link set "$SERVER_IFACE_C" up

sudo ip addr replace ${SERVER_CANONICAL_IP}/32 dev lo

echo "server addressing done:"
ip -brief addr show
