#!/usr/bin/env bash
# Run on both SERVER and CLIENT. Builds picotls then picoquic.
# NOTE: picoquic's docs pin picotls to a specific tested commit that
# changes over time - check picoquic's README/CMakeLists for the current
# recommendation before relying on picotls' own default branch tip.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -d picotls ]; then
  git clone --recursive https://github.com/h2o/picotls.git
fi
cd picotls
cmake .
make -j"$(nproc)"
cd ..

if [ ! -d picoquic ]; then
  git clone https://github.com/private-octopus/picoquic.git
fi
cd picoquic
git log -1 --format='building picoquic at commit %H (%cd)' --date=short
cmake .
make -j"$(nproc)"

./picoquicdemo -h 2>&1 | head -5 || true
