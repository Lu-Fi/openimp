#!/bin/sh
# #299: OpenIMP's T32 move / base-move interfaces against vendor libimp 2.2.0,
# frame for frame (tools/t32/t32_misc_sample.c ivsdirect, synthetic frames).
# Runs the sample under qemu-user with build/t32/libimp.so; the vendor output
# came from QEMU -M ingenic-t32 (tests/t32/vendor-2.2.0-ivs-direct*.txt).
#   T32_BUILD=<thingino T32 output> QEMU_USER=<qemu-mipsel> ROOTFS=<T32 rootfs> tools/t32/ivs_direct_check.sh
# OPENIMP_MOTION_V2=0: the vendor algorithm (motion v2 is OpenIMP's default, beyond vendor).
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
: "${T32_BUILD:?}" "${QEMU_USER:?}" "${ROOTFS:?}"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/root"; cp -a "$ROOTFS/lib" "$ROOTFS/usr" "$W/root/"
cp "$REPO/build/t32/libimp.so" "$W/root/usr/lib/libimp.so"
"$T32_BUILD/host/bin/mipsel-linux-gcc" -std=gnu99 -O1 -Wall -Wextra -Werror -DPLATFORM_T32 -I"$REPO/include" \
    -o "$W/sample" "$REPO/tools/t32/t32_misc_sample.c" -L"$T32_BUILD/target/usr/lib" -L"$T32_BUILD/target/lib" \
    -Wl,--no-as-needed -limp -lalog -lsysutils -luclibcshim -lpthread -lrt -ldl -lm -Wl,--allow-shlib-undefined
run() { env OPENIMP_MOTION_V2=0 "$@" "$QEMU_USER" -L "$W/root" "$W/sample" ivsdirect 30 | grep '^T32V'; }
run | grep -v "param base" > "$W/1.txt"   # vector 1 predates that line
grep -v '^#' "$REPO/tests/t32/vendor-2.2.0-ivs-direct.txt" | diff - "$W/1.txt"
run T32_IVS_SENSE=2 T32_BASE_SENSE=1 T32_IVS_SKIP=1 T32_BASE_REF=2 > "$W/2.txt"
grep -v '^#' "$REPO/tests/t32/vendor-2.2.0-ivs-direct2.txt" | diff - "$W/2.txt"
echo "IVS move / base move: 2 x 30 frames identical to vendor 2.2.0"
