#!/usr/bin/env bash
# Run on CLIENT. Usage: ./scripts/run-traffic-client.sh [cc_algo=cubic] [duration_sec=30]
# Writes a qlog per run to qlogs_mptraffic/client_<cc>_<timestamp>/ and a
# results/mptraffic_client_<cc>_<timestamp>.log with:
#   - mp_traffic's own stdout/stderr
#   - per-path (A/B/C) RX/TX byte deltas across the run window, the
#     project's established trusted signal for traffic distribution
#     (same /sys/class/net/*/statistics/*_bytes counters run-client-sweep.sh
#     uses)
#   - a post-run per-path latency/loss sample via the same `ping -I
#     <src_ip>` pattern verify-link.sh already uses for link
#     characterization - taken right after the run (not concurrent with
#     it, so it doesn't add load on top of the measurement itself)
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

cc="${1:-cubic}"
duration="${2:-30}"
ts="$(date +%Y%m%d_%H%M%S)"
qlog_dir="qlogs_mptraffic/client_${cc}_${ts}"
log_file="results/mptraffic_client_${cc}_${ts}.log"

idx() { ip -o link show "$1" | cut -d: -f1 | tr -d ' '; }
IDX_B=$(idx "$CLIENT_IFACE_B")
IDX_C=$(idx "$CLIENT_IFACE_C")

mkdir -p "$qlog_dir" results

rx_bytes() { cat "/sys/class/net/$1/statistics/rx_bytes"; }
tx_bytes() { cat "/sys/class/net/$1/statistics/tx_bytes"; }

{
  echo "=== path byte counters BEFORE run ==="
  for label_iface in "A:$CLIENT_IFACE_A" "B:$CLIENT_IFACE_B" "C:$CLIENT_IFACE_C"; do
    label="${label_iface%%:*}"; iface="${label_iface##*:}"
    echo "  Link $label ($iface): rx=$(rx_bytes "$iface") tx=$(tx_bytes "$iface")"
  done

  ( cd traffic-app && exec ./mp_traffic -A "${LINK_B_CLIENT_IP}/${IDX_B},${LINK_C_CLIENT_IP}/${IDX_C}" \
      -G "$cc" --duration "$duration" -q "../$qlog_dir" "$SERVER_CANONICAL_IP" "$QUIC_PORT" )

  echo "=== path byte counters AFTER run ==="
  for label_iface in "A:$CLIENT_IFACE_A" "B:$CLIENT_IFACE_B" "C:$CLIENT_IFACE_C"; do
    label="${label_iface%%:*}"; iface="${label_iface##*:}"
    echo "  Link $label ($iface): rx=$(rx_bytes "$iface") tx=$(tx_bytes "$iface")"
  done

  echo "=== post-run per-path latency/loss (ping, 20 packets each) ==="
  for label_srcip in "A:$LINK_A_CLIENT_IP" "B:$LINK_B_CLIENT_IP" "C:$LINK_C_CLIENT_IP"; do
    label="${label_srcip%%:*}"; src="${label_srcip##*:}"
    echo "-- Link $label (src $src) --"
    ping -c 20 -I "$src" "$SERVER_CANONICAL_IP" || true
  done
} 2>&1 | tee "$log_file"
