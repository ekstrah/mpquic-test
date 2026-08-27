#!/usr/bin/env bash
# Tells NetworkManager to stop managing (and stop DHCP-retrying on) the
# six bench-rig link interfaces, on whichever box this runs on. Safe to
# run on both SERVER and CLIENT - interfaces that don't exist on this box
# are silently skipped. Runtime-only (nmcli, no daemon restart, so no risk
# to your current SSH session) but does NOT survive a reboot - re-run this
# after every reboot, before net-server.sh/net-client.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

for ifc in "$SERVER_IFACE_A" "$SERVER_IFACE_B" "$SERVER_IFACE_C" "$CLIENT_IFACE_A" "$CLIENT_IFACE_B" "$CLIENT_IFACE_C"; do
  sudo nmcli device set "$ifc" managed no 2>/dev/null && echo "unmanaged: $ifc" || true
done

echo "Done. NetworkManager will stop touching these interfaces until next reboot."
