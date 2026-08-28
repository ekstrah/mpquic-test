#!/usr/bin/env bash
# Run on both SERVER and CLIENT, after scripts/build-picoquic.sh has
# already built picoquic in place (traffic-app/CMakeLists.txt reuses that
# checkout's own CMake project rather than re-deriving its OpenSSL/picotls
# dependency resolution - see the comment there).
set -euo pipefail
cd "$(dirname "$0")/.."

cd traffic-app
cmake .
make -j"$(nproc)"

./mp_traffic 2>&1 | head -3 || true
