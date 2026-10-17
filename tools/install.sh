#!/usr/bin/env bash
# Copies the built proxy DLL into the game folder (or removes it with --uninstall).
set -euo pipefail

GAME_DIR="${NIER_DIR:-$HOME/.local/share/Steam/steamapps/common/NieRAutomata}"
PROXY="${NIER_VR_PROXY:-winmm}"
BUILD_DIR="$(cd "$(dirname "$0")/.." && pwd)/build"

# The bracket keeps the pattern from matching this script's own command line.
if pgrep -f '[N]ieRAutomata\.exe' >/dev/null; then
    echo "NieR:Automata is running; quit it first." >&2
    exit 1
fi

if [[ "${1:-}" == "--uninstall" ]]; then
    rm -f "$GAME_DIR/$PROXY.dll" "$GAME_DIR/openxr_loader.dll"
    echo "Removed $PROXY.dll and openxr_loader.dll from $GAME_DIR"
    exit 0
fi

install -m 755 "$BUILD_DIR/$PROXY.dll" "$GAME_DIR/$PROXY.dll"
install -m 755 "$BUILD_DIR/openxr_loader.dll" "$GAME_DIR/openxr_loader.dll"
echo "Installed $PROXY.dll -> $GAME_DIR"
echo "Steam launch options: PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 WINEDLLOVERRIDES=\"$PROXY=n,b\" %command%"
echo "Log: $GAME_DIR/NieR-VR.log"
