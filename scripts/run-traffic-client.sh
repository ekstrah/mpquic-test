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
# Set SKIP_PING=1 to omit the ~60s ping phase - needed when looping this
# script over several CC algorithms back-to-back, since run-traffic-server.sh's
# safety-net timeout (duration + 30s) is sized for the run alone and would
# otherwise time out waiting for the next client connection while this
# script is still busy pinging from the previous iteration.
# Set SKIP_QLOG=1 to omit qlog output - qlog size scales with duration
# (a 30s run produces ~30-37MB; an hour-long run would be ~120x that,
# multiple GB per run), so long runs where you only need the delivery
# counters/NIC byte deltas should skip it.
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

cc="${1:-cubic}"
duration="${2:-30}"
ts="$(date +%Y%m%d_%H%M%S)"
qlog_dir="qlogs_mptraffic/client_${cc}_${ts}"
log_file="results/mptraffic_client_${cc}_${ts}.log"
qlog_args=()
if [ "${SKIP_QLOG:-0}" != "1" ]; then
  qlog_args=(-q "../$qlog_dir")
fi

idx() { ip -o link show "$1" | cut -d: -f1 | tr -d ' '; }
IDX_B=$(idx "$CLIENT_IFACE_B")
IDX_C=$(idx "$CLIENT_IFACE_C")

mkdir -p results
if [ "${SKIP_QLOG:-0}" != "1" ]; then
  mkdir -p "$qlog_dir"
fi

rx_bytes() { cat "/sys/class/net/$1/statistics/rx_bytes"; }
tx_bytes() { cat "/sys/class/net/$1/statistics/tx_bytes"; }

{
  echo "=== path byte counters BEFORE run ==="
  for label_iface in "A:$CLIENT_IFACE_A" "B:$CLIENT_IFACE_B" "C:$CLIENT_IFACE_C"; do
    label="${label_iface%%:*}"; iface="${label_iface##*:}"
    echo "  Link $label ($iface): rx=$(rx_bytes "$iface") tx=$(tx_bytes "$iface")"
  done

  ( cd traffic-app && exec ./mp_traffic -A "${LINK_B_CLIENT_IP}/${IDX_B},${LINK_C_CLIENT_IP}/${IDX_C}" \
      -G "$cc" --duration "$duration" "${qlog_args[@]}" "$SERVER_CANONICAL_IP" "$QUIC_PORT" )

  echo "=== path byte counters AFTER run ==="
  for label_iface in "A:$CLIENT_IFACE_A" "B:$CLIENT_IFACE_B" "C:$CLIENT_IFACE_C"; do
    label="${label_iface%%:*}"; iface="${label_iface##*:}"
    echo "  Link $label ($iface): rx=$(rx_bytes "$iface") tx=$(tx_bytes "$iface")"
  done

  if [ "${SKIP_PING:-0}" != "1" ]; then
    echo "=== post-run per-path latency/loss (ping, 20 packets each) ==="
    for label_srcip in "A:$LINK_A_CLIENT_IP" "B:$LINK_B_CLIENT_IP" "C:$LINK_C_CLIENT_IP"; do
      label="${label_srcip%%:*}"; src="${label_srcip##*:}"
      echo "-- Link $label (src $src) --"
      ping -c 20 -I "$src" "$SERVER_CANONICAL_IP" || true
    done
  fi
} 2>&1 | tee "$log_file"
