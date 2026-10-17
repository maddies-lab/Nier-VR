#!/usr/bin/env bash
# Copies the built proxy DLL into the game folder (or removes it with --uninstall).
set -euo pipefail

GAME_DIR="${NIER_DIR:-$HOME/.local/share/Steam/steamapps/common/NieRAutomata}"
PROXY="${NIER_VR_PROXY:-winmm}"
BUILD_DIR="$(cd "$(dirname "$0")/.." && pwd)/build"

if [[ "${1:-}" == "--uninstall" ]]; then
    rm -f "$GAME_DIR/$PROXY.dll"
    echo "Removed $GAME_DIR/$PROXY.dll"
    exit 0
fi

install -m 755 "$BUILD_DIR/$PROXY.dll" "$GAME_DIR/$PROXY.dll"
echo "Installed $PROXY.dll -> $GAME_DIR"
echo "Steam launch options: WINEDLLOVERRIDES=\"$PROXY=n,b\" %command%"
echo "Log: $GAME_DIR/NieR-VR.log"
