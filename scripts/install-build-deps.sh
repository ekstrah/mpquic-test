#!/usr/bin/env bash
# Run on both SERVER and CLIENT. picoquic is CMake + C, not Cargo -
# no rustup needed unlike the TQUIC rig.
set -euo pipefail
sudo apt-get update
sudo apt-get install -y build-essential pkg-config cmake git libssl-dev
cmake --version
gcc --version
