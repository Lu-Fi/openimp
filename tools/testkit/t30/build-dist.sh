#!/bin/sh
# Build the T30 test kit distribution tarball (host side, no binaries are committed).
#
#   THINGINO_TARGET_DIR=/path/to/thingino-out/<any uclibc xburst1 camera build> \
#   T30_HEADERS=/path/to/ingenic-headers/T30/1.0.5/zh \
#   APITEST_T30=/path/to/apitest_t30 \
#   ./build-dist.sh OUTDIR
#
# apitest_t30 comes from tools/apitest (branch claude/imgfx-tool): ./build.sh T30
# THINGINO_TARGET_DIR needs host/bin/mipsel-linux-gcc (thingino uclibc toolchain, MIPS32r2).
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../../.." && pwd)
outdir=${1:?usage: build-dist.sh OUTDIR}
tdir=${THINGINO_TARGET_DIR:?set THINGINO_TARGET_DIR}
hdr=${T30_HEADERS:?set T30_HEADERS (ingenic-headers/T30/1.0.5/zh)}
apitest=${APITEST_T30:?set APITEST_T30}
cc=$tdir/host/bin/mipsel-linux-gcc
strip=$tdir/host/bin/mipsel-linux-strip

mkdir -p "$outdir"
THINGINO_DIR=${THINGINO_DIR:-/nonexistent} T30_TARGET_DIR=$tdir "$root/build-t30.sh"
stage=$outdir/t30kit
rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/lib"
"$cc" -O2 -Wall -Wextra -DPLATFORM_T30 -I"$hdr" -I"$hdr/imp" "$here/t30_encode.c" \
    -o "$stage/bin/t30_encode" -L"$root/build/t30" -limp -Wl,-rpath-link,"$root/build/t30"
cp "$apitest" "$stage/bin/apitest_t30"
"$strip" "$stage/bin/t30_encode" "$stage/bin/apitest_t30"
cp "$root/build/t30/libimp.so" "$stage/lib/libimp.so"
cp "$here/run.sh" "$here/README.md" "$stage/"
chmod +x "$stage/run.sh" "$stage/bin/"*
( cd "$stage" && sha256sum run.sh bin/* lib/* >SHA256SUMS )
rev=$(git -C "$root" rev-parse --short HEAD)
name=openimp-t30-testkit-$rev
tar -C "$outdir" -czf "$outdir/$name.tar.gz" t30kit
( cd "$outdir" && sha256sum "$name.tar.gz" >"$name.tar.gz.sha256" && cat "$name.tar.gz.sha256" )
