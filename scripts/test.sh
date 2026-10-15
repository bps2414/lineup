#!/usr/bin/env bash
# Host self-test of the lineupd service (needs clang). Output in out/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="${CC:-clang-18}"
command -v "$CC" >/dev/null || CC=clang
LOCK="$ROOT/ps5-react/dependencies.lock.json"
SQL="$ROOT/ps5-react/.deps/sqlite-amalgamation-3460100"
OUT="$ROOT/out"
mkdir -p "$OUT"

if [ ! -f "$SQL/sqlite3.c" ]; then
  read -r URL SHA < <(python3 -c "import json,sys;d=json.load(open(sys.argv[1]))['sqlite'];print(d['url'],d['sha256'])" "$LOCK")
  curl -fsSL "$URL" -o "$OUT/sqlite.zip"
  echo "$SHA  $OUT/sqlite.zip" | sha256sum -c -
  SQL="$OUT/sqlite-amalgamation-3460100"
  rm -rf "$SQL"; unzip -q -o "$OUT/sqlite.zip" -d "$OUT"
fi

$CC -O1 -c -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
  "$SQL/sqlite3.c" -o "$OUT/sqlite3.o"
$CC -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -DLINEUPD_TEST \
  -I "$SQL" "$ROOT/app/payloads/lineupd.c" "$OUT/sqlite3.o" -o "$OUT/lineupd-test"
"$OUT/lineupd-test"
