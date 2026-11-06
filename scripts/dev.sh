#!/usr/bin/env bash
# One command for design sessions: rebuilds and starts the simulator plus the live web UI dev server.
# Touchscreen window + http://localhost:5173/ - right-click either to leave a note in annotations/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# The simulator build only needs these once; firmware builds generate version.h, this one does not.
if [ ! -f src/version.h ]; then
  VERSION="$(git describe --tags --dirty --match 'v*' 2>/dev/null || echo dev)"
  STAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf '#pragma once\n#define BUILD_GIT_VERSION "%s"\n#define BUILD_TIMESTAMP "%s"\n' "$VERSION" "$STAMP" > src/version.h
fi

export NVM_DIR="$HOME/.nvm"
set +eu
. "$NVM_DIR/nvm.sh" --no-use
nvm use >/dev/null
set -eu

[ -f src/display/webassets/web_ui.bin ] || scripts/build_webui.sh
[ -d web/node_modules ] || (cd web && npm ci)

"$HOME/.platformio/penv/bin/pio" run -e display-sim

./.pio/build/display-sim/program &
SIM_PID=$!
trap 'kill "$SIM_PID" 2>/dev/null || true' EXIT

cd web
npm run dev
