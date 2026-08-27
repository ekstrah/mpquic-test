#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh
sudo ip rule del from ${LINK_A_CLIENT_IP} table 101 2>/dev/null || true
sudo ip rule del from ${LINK_B_CLIENT_IP} table 102 2>/dev/null || true
sudo ip rule del from ${LINK_C_CLIENT_IP} table 103 2>/dev/null || true
sudo ip route flush table 101 2>/dev/null || true
sudo ip route flush table 102 2>/dev/null || true
sudo ip route flush table 103 2>/dev/null || true
echo "policy routing cleared. Interface addresses left in place."
