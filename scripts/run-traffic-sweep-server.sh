#!/usr/bin/env bash
# Run on SERVER. Usage: ./scripts/run-traffic-sweep-server.sh [duration_sec=30]
# Loops run-traffic-server.sh over every algorithm in SWEEP_CC_LIST
# (env.sh), in order - pair with run-traffic-sweep-client.sh on the
# other box, started a few seconds later so its own loop stays roughly
# in sync.
#
# Respects SKIP_QLOG (see run-traffic-server.sh) if exported first - for
# long durations, qlog size scales with duration (a 30s run is
# ~30-37MB; an hour-long run would be multiple GB per algorithm), e.g.:
#   SKIP_QLOG=1 ./scripts/run-traffic-sweep-server.sh 3600
#
# To start from a clean set of results first:
#   rm -f results/mptraffic_server_*.log
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

duration="${1:-30}"

for cc in "${SWEEP_CC_LIST[@]}"; do
  echo "=== mp_traffic sweep: server side, cc=$cc, duration=${duration}s ==="
  ./scripts/run-traffic-server.sh "$cc" "$duration"
done

echo "mp_traffic sweep done (server side)."
