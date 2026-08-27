#!/usr/bin/env bash
# Run on CLIENT in a separate terminal, before starting the multipath client.
# Output is both printed and appended to capture-all-links.log for tmux
# scrollback-free review (tail -f capture-all-links.log in another pane).
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

{
  echo "=== $(date -Is) ==="
  echo "packet counts:"
  for ifc in "$CLIENT_IFACE_A" "$CLIENT_IFACE_B" "$CLIENT_IFACE_C"; do
    echo "--- $ifc ---"
    ip -s link show "$ifc"
  done
  echo "run the multipath client now in another terminal, then re-run this script."
} | tee -a capture-all-links.log
