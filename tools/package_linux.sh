#!/bin/sh
# package_linux.sh - the portable Linux release archive for GitHub, as package_release.ps1 makes the Windows zip:
# an explicit list of files (the game and libSDL3.so.0 from the build folder, the docs, the licences), so
# settings, logs, saves and anything from the disc never end up in it; dist/SHA256SUMS.txt gets its line.
#   tools/package_linux.sh <version> <source url>     (OUT = the build folder, default build-linux; DIST = dist/)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build-linux}
VERSION=${1:?usage: package_linux.sh <version> <source url>}
SOURCE=${2:?usage: package_linux.sh <version> <source url>}
echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.]+)?$' || { echo "bad version: $VERSION"; exit 1; }
echo "$SOURCE" | grep -Eq '^https://[^[:space:]|&]+$' || { echo "bad source url (GPL: where the source is): $SOURCE"; exit 1; }
NAME=TeamBuddies-$VERSION-linux-x86_64
DIST=${DIST:-$ROOT/dist}

# the binaries must exist and be newer than every runtime source
for f in TeamBuddies libSDL3.so.0; do [ -f "$OUT/$f" ] || { echo "missing $OUT/$f - run build.sh first"; exit 1; }; done
newer=$(find "$ROOT/runtime" -type f -newer "$OUT/TeamBuddies" | head -n 1)
[ -z "$newer" ] || { echo "$newer is newer than the game - run build.sh runtime first"; exit 1; }
grep -Eq "^## $VERSION( |\$)" "$ROOT/CHANGELOG.md" || echo "warning: CHANGELOG.md has no '## $VERSION' section"

# stage
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
D=$STAGE/$NAME
mkdir -p "$D/licenses"
cp "$OUT/TeamBuddies" "$OUT/libSDL3.so.0" "$ROOT/CHANGELOG.md" "$ROOT/LICENSE" "$ROOT/docs/PLAYING.md" "$D/"
cp "$ROOT/licenses/"* "$D/licenses/"
sed -e "s|{VERSION}|$VERSION|g" -e "s|{SOURCE}|$SOURCE|g" "$ROOT/tools/release/README-linux.txt" | tr -d '\r' > "$D/README.txt"
find "$D" -type f -exec chmod 644 {} +
chmod 755 "$D/TeamBuddies"

# exactly these files, nothing else
{ printf '%s\n' TeamBuddies libSDL3.so.0 CHANGELOG.md LICENSE PLAYING.md README.txt; ls "$ROOT/licenses" | sed 's|^|licenses/|'; } | sort > "$STAGE/expected"
(cd "$D" && find . -type f | sed 's|^\./||' | sort) > "$STAGE/actual"
cmp -s "$STAGE/expected" "$STAGE/actual" || { echo "unexpected package contents:"; diff "$STAGE/expected" "$STAGE/actual"; exit 1; }

# archive into a temp file, then swap it in (a failed run never leaves a half-written archive)
mkdir -p "$DIST"
tar -C "$STAGE" --sort=name --owner=0 --group=0 --numeric-owner -czf "$DIST/$NAME.tar.gz.partial" "$NAME"
mv "$DIST/$NAME.tar.gz.partial" "$DIST/$NAME.tar.gz"

# SHA256SUMS.txt: this archive's line replaced, the other packages' kept
hash=$(sha256sum "$DIST/$NAME.tar.gz" | cut -d' ' -f1)
{ if [ -f "$DIST/SHA256SUMS.txt" ]; then grep -v "  $NAME.tar.gz\$" "$DIST/SHA256SUMS.txt" || true; fi
  echo "$hash  $NAME.tar.gz"; } > "$DIST/SHA256SUMS.txt.tmp"
mv "$DIST/SHA256SUMS.txt.tmp" "$DIST/SHA256SUMS.txt"
echo "$DIST/$NAME.tar.gz  $(du -h "$DIST/$NAME.tar.gz" | cut -f1)"
echo "$DIST/SHA256SUMS.txt  $hash"
