#!/usr/bin/env bash
# Decompile functions from the analyzed project: dump_at.sh <out-dir> <depth> <address>...
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GHIDRA_HEADLESS_MAXMEM=10G /opt/ghidra/support/analyzeHeadless "$ROOT/re/ghidra" NieRAutomata \
    -process NieRAutomata.decrypted.exe -noanalysis -readOnly \
    -scriptPath "$ROOT/tools/ghidra" -postScript DumpAt.java "$@" 2>&1 | grep -E "DumpAt|ERROR|Exception" || true
