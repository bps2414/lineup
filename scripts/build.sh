#!/usr/bin/env bash
# Build Lineup for the PS5: sets up ps5-react (npm ci, python venv) then runs its builder on ./app.
# Output: ps5-react/dist/PPSA99077/. Override the LLVM bin dir with LLVM=/path/to/llvm/bin.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLVM="${LLVM:-/usr/lib/llvm-18/bin}"
cd "$ROOT/ps5-react"
[ -f tools/build_ps5.py ] || { echo "ps5-react/tools/build_ps5.py is missing" >&2; exit 1; }

[ -d node_modules ] || npm ci --omit=optional
if [ ! -x .venv/bin/python3 ]; then
  python3 -m venv .venv
  .venv/bin/pip install -r requirements.txt
fi
. .venv/bin/activate
export PATH="$LLVM:$PATH"
python3 tools/build_ps5.py --app-dir "$ROOT/app" --llvm "$LLVM"
