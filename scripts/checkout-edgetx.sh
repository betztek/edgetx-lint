#!/usr/bin/env bash
# Check out an EdgeTX release tag in the third_party/edgetx submodule
# (shallow), including EdgeTX's own nested submodules.
# usage: scripts/checkout-edgetx.sh v2.12.5
set -euo pipefail
TAG="${1:?usage: $0 TAG}"
cd "$(dirname "$0")/../third_party/edgetx"
git fetch --depth 1 origin tag "$TAG"
git -c advice.detachedHead=false checkout -q "$TAG"
git submodule update --init --recursive --depth 1
git log --oneline -1
