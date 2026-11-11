#!/bin/sh
# T32 platform skeleton (#286, planned): PRJ007, vendor 2.x kernel ABI
# (open-tx-isp driver/t32).  XBurst1 MIPS32r2 + MXU2 like T31, uClibc.
# Builds libimp.so and libimp.a from the OpenIMP headers only; the entry
# points are stubs until the T32 issues land (docs/FEATURE_MATRIX: planned).
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output_dir=${T32_OUTPUT_DIR:-"$project_dir/build/t32"}

# Toolchain: TOOLCHAIN_PREFIX, else a Thingino T32 build (output/<profile>/*t32*
# in THINGINO_DIR, or a thingino-out/*t32* build directory next to the repo).
if [ -z "${TOOLCHAIN_PREFIX:-}" ]; then
    for candidate in \
        ${THINGINO_DIR:+"$THINGINO_DIR"/output/*/*t32*/host/bin/mipsel-linux-gcc} \
        "$project_dir"/../thingino-out/*t32*/host/bin/mipsel-linux-gcc \
        "$project_dir"/../../thingino-out/*t32*/host/bin/mipsel-linux-gcc \
        "$project_dir"/../../../thingino-out/*t32*/host/bin/mipsel-linux-gcc
    do
        if [ -x "$candidate" ]; then
            TOOLCHAIN_PREFIX=${candidate%-gcc}
            break
        fi
    done
fi
: "${TOOLCHAIN_PREFIX:?set TOOLCHAIN_PREFIX to <T32 build>/host/bin/mipsel-linux}"
compiler="${TOOLCHAIN_PREFIX}-gcc"
archiver="${TOOLCHAIN_PREFIX}-ar"
test -x "$compiler"
mkdir -p "$output_dir"

case "${OPENIMP_SIZE_OPT:-1}" in
    1) opt_flags=-Os ;;
    0) opt_flags=-O2 ;;
    *) echo "OPENIMP_SIZE_OPT must be 0 or 1" >&2; exit 1 ;;
esac
case "${OPENIMP_DEBUG:-0}" in 0|1) ;; *) echo "OPENIMP_DEBUG must be 0 or 1" >&2; exit 1 ;; esac
base_flags="-std=gnu99 $opt_flags -mabi=32 -march=mips32r2 -mabicalls"
base_flags="$base_flags -fPIC -G0 -fno-stack-protector -DPLATFORM_T32"
base_flags="$base_flags -ffunction-sections -fdata-sections"
base_flags="$base_flags -DOPENIMP_DEBUG=${OPENIMP_DEBUG:-0}"
strict_flags="$base_flags -Wall -Wextra -Werror"
repo_includes="-I$project_dir/include -I$project_dir/src"

objects=
compile()
{
    object=$1
    source=$2
    "$compiler" $strict_flags $repo_includes \
        -c "$project_dir/$source" -o "$output_dir/$object.o"
    objects="$objects $output_dir/$object.o"
}

# IMP_LOG back end and IMP_Log_* (vendor libalog), shared with every SoC
compile core_imp_log_emit src/core/imp_log_emit.c
compile core_imp_log src/core/imp_log.c
# reserved-arena DMA allocator (shared with T21/T23/T30/T31)
compile dma_alloc src/dma_alloc.c
# T32 layout asserts + stub entry points
compile t32_skeleton src/t32/openimp_t32_skeleton.c
# ISP open / sensor lifecycle, System_Init/Exit (#288)
compile t32_isp src/t32/openimp_t32_isp.c
# userspace IQ-bin loader, day/night bins (#289)
compile t32_bin src/t32/openimp_t32_bin.c

rm -f "$output_dir/libimp.a"
"$archiver" rcs "$output_dir/libimp.a" $objects

"$compiler" -shared -nostartfiles \
    -Wl,-soname,libimp.so -Wl,--gc-sections \
    -Wl,--version-script="$project_dir/src/t40/libimp.map" \
    -o "$output_dir/libimp.so" $objects \
    -ldl -lpthread -lrt

if readelf -d "$output_dir/libimp.so" |
    grep -q 'Shared library: \[libimp.so'
then
    echo "T32 build has an OEM libimp dependency" >&2
    exit 1
fi

echo "T32 skeleton: $(readelf --dyn-syms --wide "$output_dir/libimp.so" |
    awk '$7 != "UND" && $5 == "GLOBAL" && $8 ~ /^IMP_/' | wc -l) IMP_ exports"
sha256sum "$output_dir/libimp.so" "$output_dir/libimp.a"
readelf -d "$output_dir/libimp.so" | grep -E 'SONAME|NEEDED'
