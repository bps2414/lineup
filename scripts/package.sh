#!/usr/bin/env bash
# Package the built app into dist/: exFAT image (ShadowMountPlus), FTP zip, source tarball, SHA256SUMS.
# Usage: scripts/package.sh <version> [--no-exfat]   (exFAT step needs sudo, mkfs.exfat from exfatprogs)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:?usage: package.sh <version> [--no-exfat]}"
NO_EXFAT="${2:-}"
SRC="$ROOT/ps5-react/dist/PPSA99077"
DIST="$ROOT/dist"
[ -f "$SRC/eboot.bin" ] || { echo "run scripts/build.sh first" >&2; exit 1; }
rm -rf "$DIST"; mkdir -p "$DIST"

# FTP install zip: PPSA99077/ -> /data/homebrew/PPSA99077
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
cp -r "$SRC" "$STAGE/PPSA99077"
(cd "$STAGE" && zip -qr "$DIST/Lineup-$VERSION.zip" PPSA99077)

# Source tarball incl. the ps5-react submodule (GPL corresponding source), no build outputs.
(cd "$ROOT" && { git ls-files; git submodule foreach --recursive --quiet 'git ls-files | sed "s|^|$displaypath/|"'; } \
  | sort -u | while IFS= read -r f; do { [ -f "$f" ] && echo "$f" || true; }; done > "$STAGE/files.txt"
  tar --transform "s|^|Lineup-$VERSION/|" -czf "$DIST/Lineup-$VERSION-source.tar.gz" -T "$STAGE/files.txt")

# exFAT image: app at the image root, 64K clusters, size a multiple of 64 KiB.
if [ "$NO_EXFAT" != "--no-exfat" ]; then
  IMG="$DIST/Lineup-$VERSION.exfat"
  KB=$(du -sk "$SRC" | cut -f1)
  SIZE_K=$(( ((KB + KB / 10 + 16384 + 63) / 64) * 64 ))   # +10% +16 MiB slack, rounded to 64 KiB
  truncate -s "${SIZE_K}K" "$IMG"
  mkfs.exfat -c 64K -L LINEUP "$IMG" >/dev/null
  MNT="$(mktemp -d)"
  sudo mount -o loop,uid="$(id -u)",gid="$(id -g)" "$IMG" "$MNT"
  cp -r "$SRC"/. "$MNT"/
  sync
  sudo umount "$MNT"; rmdir "$MNT"
fi

(cd "$DIST" && sha256sum -- * > SHA256SUMS.tmp && mv SHA256SUMS.tmp SHA256SUMS)
ls -l "$DIST"
