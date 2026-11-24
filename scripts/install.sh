#!/usr/bin/env bash
# Download the prebuilt linux-x64 release instead of building EdgeTX
# (useful in Claude Code cloud sessions and CI).
#
# usage: scripts/install.sh [TAG|latest] [PREFIX]
#   default PREFIX: ~/.local/edgetx-lint ; then run PREFIX/bin/edgetx-lint
# Set GITHUB_TOKEN if the repository is private.
set -euo pipefail
TAG="${1:-latest}"
PREFIX="${2:-$HOME/.local/edgetx-lint}"
REPO="betztek/edgetx-lint"
ASSET="edgetx-lint-linux-x64.tar.gz"
if [ "$TAG" = latest ]; then
  URL="https://github.com/$REPO/releases/latest/download/$ASSET"
else
  URL="https://github.com/$REPO/releases/download/$TAG/$ASSET"
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AUTH=()
[ -n "${GITHUB_TOKEN:-}" ] && AUTH=(-H "Authorization: Bearer $GITHUB_TOKEN")
curl -fsSL "${AUTH[@]}" -o "$TMP/$ASSET" "$URL"
curl -fsSL "${AUTH[@]}" -o "$TMP/$ASSET.sha256" "$URL.sha256"
(cd "$TMP" && sha256sum -c "$ASSET.sha256")
mkdir -p "$PREFIX"
tar -C "$PREFIX" -xzf "$TMP/$ASSET"
"$PREFIX/bin/edgetx-lint" list
echo "installed: $PREFIX/bin/edgetx-lint"
