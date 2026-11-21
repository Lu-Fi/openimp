#!/bin/bash
# export-check: built libimp.so exports vs vendor export list vs IMP_ names used by streamers (see tools/export-check/README.md)
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
ec=$here/tools/export-check
NM=${NM:-nm}
SOCS=${SOCS:-"T10 T20 T21 T23 T30 T31 T40 T41"}
# timps (src/hal/libimp_check.h) treats a libimp as OpenIMP when ANY of these is
# exported; OpenIMP_P0_GetState is the one every SoC build exports (#545), so a
# build without it makes an OpenIMP-flavour timpsd refuse to start (exit 78).
OPENIMP_MARKER=${OPENIMP_MARKER:-OpenIMP_P0_GetState}
STREAMER_DIRS=${STREAMER_DIRS:-}
BUILD=${BUILD:-$here/build}
if [ -z "$STREAMER_DIRS" ]; then
  for d in "$here/../timps" "$here/../prudynt-t" "$here/../raptor"; do [ -d "$d" ] && STREAMER_DIRS="$STREAMER_DIRS $d"; done
fi
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
: > "$tmp/raw"
for d in $STREAMER_DIRS; do
  [ -d "$d" ] || { echo "export-check: streamer dir $d missing" >&2; continue; }
  find "$d" \( -name .git -o -name include -o -name ingenic-headers -o -name 3rdparty -o -name build \) -prune -o -type f \
       \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' -o -name '*.hpp' \) -print0 |
    xargs -0 -r grep -ohE '\bIMP_[A-Za-z0-9_]+' >> "$tmp/raw" 2>/dev/null
done
sort -u "$tmp/raw" > "$tmp/streamer"
echo "export-check: $(wc -l < "$tmp/streamer") IMP_ names in streamer sources (${STREAMER_DIRS:-none})"
rc=0
for soc in $SOCS; do
  lc=$(echo "$soc" | tr A-Z a-z); lib=$BUILD/$lc/libimp.so; vend=$ec/vendor-$soc.txt
  [ "$soc" = T10 ] && [ ! -f "$lib" ] && lib=$BUILD/t20/libimp.so   # T10 ships the T20 build
  if [ ! -f "$lib" ]; then
    echo "$soc: no $lib (build it first)"; [ -n "${EXPORT_CHECK_STRICT:-}" ] && rc=1; continue
  fi
  $NM -D --defined-only "$lib" 2>/dev/null | awk '$2 ~ /^[TWDBR]$/ && $3 ~ /^IMP_/ {print $3}' | sort -u > "$tmp/open"
  if ! $NM -D --defined-only "$lib" 2>/dev/null | awk '{print $3}' | grep -qx "$OPENIMP_MARKER"; then
    echo "  FAIL $soc: $OPENIMP_MARKER not exported (timps identity marker, #545)"; rc=1
  fi
  sed 's/[[:space:]].*//' "$vend" | grep '^IMP_' | sort -u > "$tmp/vend"
  ign=$ec/streamer-ignore-$soc.txt
  { [ -f "$ign" ] && sed 's/#.*//; s/[[:space:]]//g' "$ign" | grep -v '^$' || true; } | sort -u > "$tmp/ign"
  comm -23 "$tmp/vend" "$tmp/open" > "$tmp/miss"
  comm -12 "$tmp/miss" "$tmp/streamer" | comm -23 - "$tmp/ign" > "$tmp/bad"
  comm -12 "$tmp/miss" "$tmp/streamer" | comm -12 - "$tmp/ign" > "$tmp/ignored"
  echo "$soc: vendor $(wc -l < "$tmp/vend"), exported $(comm -12 "$tmp/vend" "$tmp/open" | wc -l), missing $(wc -l < "$tmp/miss")," \
       "streamer-used missing $(wc -l < "$tmp/bad") (ignored $(wc -l < "$tmp/ignored"))"
  [ -n "${LIST:-}" ] && sed 's/^/  missing: /' "$tmp/miss"
  if [ -s "$tmp/bad" ]; then sed 's/^/  FAIL streamer-used, vendor-exported, not exported: /' "$tmp/bad"; rc=1; fi
done
exit $rc
