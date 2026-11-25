#!/usr/bin/env bash
# Install what EdgeTX's native (simu) build needs on Ubuntu 24.04.
# EdgeTX's code generators use libclang (Python bindings) and Pillow/lz4.
set -euo pipefail
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO=sudo
$SUDO apt-get update -qq
$SUDO apt-get install -y -qq --no-install-recommends \
  cmake ninja-build g++ clang libclang-18-dev python3-pip
# EdgeTX <= 2.10 only searches /usr/lib and /usr/local/lib for libclang.so
$SUDO ln -sf /usr/lib/llvm-18/lib/libclang.so /usr/local/lib/libclang.so
python3 -m pip install --quiet --break-system-packages \
  "clang==18.1.8" pillow lz4 jinja2 asciitree pydantic
