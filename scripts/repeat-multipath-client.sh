#!/usr/bin/env bash
# Run on CLIENT (server must already be running with -M, see build-picoquic
# / phase 1 notes). Repeats the multipath transfer N times, capturing
# per-link client-side RX byte deltas each run - the trusted signal for
# actual traffic distribution (same reasoning as the TQUIC rig's
# Link-A-dominance finding: NIC counters over the full run, not app-level
# cwnd/send logs).
#
# Usage: ./scripts/repeat-multipath-client.sh [runs=10] [sleep_between_sec=3] [scenario=/testfile.bin]
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

RUNS="${1:-10}"
SLEEP="${2:-3}"
SCENARIO="${3:-/testfile.bin}"
OUT="results/repeat-multipath.csv"
mkdir -p results

idx() { ip -o link show "$1" | cut -d: -f1 | tr -d ' '; }
IDX_B=$(idx "$CLIENT_IFACE_B")
IDX_C=$(idx "$CLIENT_IFACE_C")

rx_bytes() { cat "/sys/class/net/$1/statistics/rx_bytes"; }

if [ ! -f "$OUT" ]; then
  echo "run,a_rx_bytes,b_rx_bytes,c_rx_bytes,a_share_pct,b_share_pct,c_share_pct,elapsed_s,mbps,exit_code" > "$OUT"
fi

for i in $(seq 1 "$RUNS"); do
  a0=$(rx_bytes "$CLIENT_IFACE_A"); b0=$(rx_bytes "$CLIENT_IFACE_B"); c0=$(rx_bytes "$CLIENT_IFACE_C")

  log="results/run_${i}.log"
  set +e
  ./picoquic/picoquicdemo -M -n test.example.com \
    -A "${LINK_B_CLIENT_IP}/${IDX_B},${LINK_C_CLIENT_IP}/${IDX_C}" \
    "$SERVER_CANONICAL_IP" "$QUIC_PORT" "$SCENARIO" > "$log" 2>&1
  code=$?
  set -e

  a1=$(rx_bytes "$CLIENT_IFACE_A"); b1=$(rx_bytes "$CLIENT_IFACE_B"); c1=$(rx_bytes "$CLIENT_IFACE_C")
  da=$((a1 - a0)); db=$((b1 - b0)); dc=$((c1 - c0))
  total=$((da + db + dc))

  if [ "$total" -gt 0 ]; then
    pa=$(awk "BEGIN{printf \"%.1f\", 100*$da/$total}")
    pb=$(awk "BEGIN{printf \"%.1f\", 100*$db/$total}")
    pc=$(awk "BEGIN{printf \"%.1f\", 100*$dc/$total}")
  else
    pa=0; pb=0; pc=0
  fi

  elapsed=$(grep -oP 'Received \d+ bytes in \K[0-9.]+(?= seconds)' "$log" || echo "")
  mbps=$(grep -oP 'seconds, \K[0-9.]+(?= Mbps)' "$log" || echo "")

  row="$i,$da,$db,$dc,$pa,$pb,$pc,$elapsed,$mbps,$code"
  echo "$row" | tee -a "$OUT"

  sleep "$SLEEP"
done

echo "done. per-run logs in results/run_*.log, summary in $OUT"
