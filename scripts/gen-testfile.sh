#!/usr/bin/env bash
# Run on SERVER. Generates a fixed-size file into the docroot the picoquic
# demo server serves, so experiment runs have real bulk data to move.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p www
dd if=/dev/urandom of=www/testfile.bin bs=1M count=5 2>/dev/null
echo "generated www/testfile.bin ($(du -h www/testfile.bin | cut -f1))"
