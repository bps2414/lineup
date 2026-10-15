#!/usr/bin/env bash
# Package the built app into dist/: exFAT image (ShadowMountPlus), FTP zip, source tarball, update.json, SHA256SUMS.
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

# Source tarball: every tracked file, framework included (GPL corresponding source), no build outputs.
(cd "$ROOT" && git ls-files | while IFS= read -r f; do { [ -f "$f" ] && echo "$f" || true; }; done > "$STAGE/files.txt"
  tar --transform "s|^|Lineup-$VERSION/|" -czf "$DIST/Lineup-$VERSION-source.tar.gz" -T "$STAGE/files.txt")

# exFAT image: app at the image root, 64K clusters, size a multiple of 64 KiB.
if [ "$NO_EXFAT" != "--no-exfat" ]; then
  IMG="$DIST/Lineup-$VERSION.exfat"
  KB=$(du -sk "$SRC" | cut -f1)
  SIZE_K=$(( ((KB + KB / 10 + 16384 + 63) / 64) * 64 ))   # +10% +16 MiB slack, rounded to 64 KiB
  truncate -s "${SIZE_K}K" "$IMG"
  mkfs.exfat -c 64K -L LINEUP "$IMG" >/dev/null
  MNT="$(mktemp -d)"
  # The kernel driver when there is one, else FUSE (GitHub's runner kernels lack the exfat module).
  sudo modprobe exfat 2>/dev/null || true
  if grep -qw exfat /proc/filesystems; then sudo mount -o loop,uid="$(id -u)",gid="$(id -g)" "$IMG" "$MNT"
  else LOOP="$(sudo losetup -f --show "$IMG")"; sudo mount.exfat-fuse -o allow_other,uid="$(id -u)",gid="$(id -g)" "$LOOP" "$MNT"; fi
  cp -r "$SRC"/. "$MNT"/
  sync
  sudo umount "$MNT"; rmdir "$MNT"; [ -z "${LOOP:-}" ] || sudo losetup -d "$LOOP"
fi

# update.json, the in-app updater's manifest. The notes are this version's CHANGELOG.md section; there is no
# Portuguese changelog, so pt-BR repeats the English text.
awk -v v="$VERSION" '/^## /{p = ($2 == v); next} p' "$ROOT/CHANGELOG.md" > "$STAGE/notes.md"
python3 - "$DIST" "$VERSION" "$STAGE/notes.md" <<'EOF'
import hashlib, json, os, sys
dist, version, notes = sys.argv[1], sys.argv[2], open(sys.argv[3], encoding="utf-8").read().strip()
def entry(name):
    digest = hashlib.sha256()
    with open(os.path.join(dist, name), "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return {"name": name, "size": os.path.getsize(os.path.join(dist, name)), "sha256": digest.hexdigest()}
manifest = {"version": version, "notes": {"en": notes, "pt-BR": notes}, "zip": entry(f"Lineup-{version}.zip")}
if os.path.exists(os.path.join(dist, f"Lineup-{version}.exfat")):
    manifest["exfat"] = entry(f"Lineup-{version}.exfat")
with open(os.path.join(dist, "update.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, ensure_ascii=False)
    f.write("\n")
EOF

(cd "$DIST" && sha256sum -- * > SHA256SUMS.tmp && mv SHA256SUMS.tmp SHA256SUMS)
ls -l "$DIST"
