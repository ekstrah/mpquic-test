#!/usr/bin/env bash
# Run on CLIENT. Usage: ./scripts/run-traffic-sweep-client.sh [duration_sec=30]
# Loops run-traffic-client.sh over every algorithm in SWEEP_CC_LIST
# (env.sh), in the same order as run-traffic-sweep-server.sh - start
# this a few seconds after that one so the two independent loops stay
# roughly synced.
#
# SKIP_PING is forced on for every iteration (not optional here) - the
# ~60s post-run ping phase would make each client iteration run
# noticeably longer than the matching server iteration's safety-net
# timeout (duration + 30s), and the two loops would drift apart after
# 1-2 algorithms. Run verify-link.sh separately if you want a fresh
# per-path latency/loss sample.
#
# Respects SKIP_QLOG (see run-traffic-client.sh) if exported first - for
# long durations, qlog size scales with duration (a 30s run is
# ~30-37MB; an hour-long run would be multiple GB per algorithm), e.g.:
#   SKIP_QLOG=1 ./scripts/run-traffic-sweep-client.sh 3600
#
# To start from a clean set of results first:
#   rm -f results/mptraffic_client_*.log
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh

duration="${1:-30}"

for cc in "${SWEEP_CC_LIST[@]}"; do
  sleep 5
  echo "=== mp_traffic sweep: client side, cc=$cc, duration=${duration}s ==="
  SKIP_PING=1 ./scripts/run-traffic-client.sh "$cc" "$duration"
done

echo "mp_traffic sweep done (client side)."
