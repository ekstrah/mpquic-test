#!/usr/bin/env bash
# Run on CLIENT. Cycles through SWEEP_CC_LIST in the same order as
# run-server-sweep.sh - no live channel between boxes, client just waits
# SWEEP_CLIENT_DELAY_SEC after each presumed server restart, then does one
# multipath GET of /testfile.bin per combo. Captures per-link client-side
# RX byte deltas (the trusted signal for actual traffic distribution, per
# the whole point of this harness - not app-level cwnd/send logs) via
# /sys/class/net/*/statistics/rx_bytes. Fresh -N/-T token/ticket files
# per combo so 0-RTT resumption state doesn't carry over between combos
# (learned the hard way in repeat-multipath-client.sh: stale cached
# transport parameters can truncate a transfer early while still exiting 0).
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

OUT="results/sweep.csv"
mkdir -p results

idx() { ip -o link show "$1" | cut -d: -f1 | tr -d ' '; }
IDX_B=$(idx "$CLIENT_IFACE_B")
IDX_C=$(idx "$CLIENT_IFACE_C")

rx_bytes() { cat "/sys/class/net/$1/statistics/rx_bytes"; }

if [ ! -f "$OUT" ]; then
  echo "cc,a_rx_bytes,b_rx_bytes,c_rx_bytes,a_share_pct,b_share_pct,c_share_pct,elapsed_s,mbps,exit_code" > "$OUT"
fi

for cc in "${SWEEP_CC_LIST[@]}"; do
  sleep "$SWEEP_CLIENT_DELAY_SEC"

  a0=$(rx_bytes "$CLIENT_IFACE_A"); b0=$(rx_bytes "$CLIENT_IFACE_B"); c0=$(rx_bytes "$CLIENT_IFACE_C")

  log="results/run_${cc}.log"
  tok=$(mktemp); tkt=$(mktemp)
  set +e
  ./picoquic/picoquicdemo -M -n test.example.com \
    -N "$tok" -T "$tkt" \
    -A "${LINK_B_CLIENT_IP}/${IDX_B},${LINK_C_CLIENT_IP}/${IDX_C}" \
    "$SERVER_CANONICAL_IP" "$QUIC_PORT" /testfile.bin > "$log" 2>&1
  code=$?
  set -e
  rm -f "$tok" "$tkt"

  a1=$(rx_bytes "$CLIENT_IFACE_A"); b1=$(rx_bytes "$CLIENT_IFACE_B"); c1=$(rx_bytes "$CLIENT_IFACE_C")
  da=$((a1 - a0)); db=$((b1 - b0)); dc=$((c1 - c0))
  total=$((da + db + dc))

  if [ "$total" -gt 0 ]; then
    pa=$(LC_NUMERIC=C awk "BEGIN{printf \"%.1f\", 100*$da/$total}")
    pb=$(LC_NUMERIC=C awk "BEGIN{printf \"%.1f\", 100*$db/$total}")
    pc=$(LC_NUMERIC=C awk "BEGIN{printf \"%.1f\", 100*$dc/$total}")
  else
    pa=0; pb=0; pc=0
  fi

  elapsed=$(grep -oP 'Received \d+ bytes in \K[0-9.]+(?= seconds)' "$log" | head -1 || echo "")
  mbps=$(grep -oP 'Received \d+ bytes in [0-9.]+ seconds, \K[0-9.]+(?= Mbps)' "$log" | head -1 || echo "")

  row="$cc,$da,$db,$dc,$pa,$pb,$pc,$elapsed,$mbps,$code"
  echo "$row" | tee -a "$OUT"
done

echo "sweep done. per-combo logs in results/run_<cc>.log, summary in $OUT"
