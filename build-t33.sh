#!/bin/sh
# T33 platform skeleton (#390, planned): PRJ008 on the vendor 3.10.14 kernel
# with the libimp 2.0.2.1 line (open-tx-isp driver/t33 = the driver/t32
# sources with TX_ISP_T33).  The T33 shares the T32 2.x ioctl ABI, so this is
# the T32 platform (PLATFORM_T32) plus PLATFORM_T33 for the measured deltas
# (audit: IMPISPBinInfo 856, tisp_bin_attr_t 828, IMPEncoderKernEncOut 48,
# PRJ008 bins, bin buffer 0x25800).  Vendor 2.0.2.1 headers for the tuning
# and ISP-OSD units.  Toolchain: the T32 one (same XBurst1 MIPS32r2 + uClibc).
# Size asserts of the three deltas: src/t32/openimp_t32_tuning.c.  WDR: the
# T32 platform exports no IMP_ISP_WDR_* (the T33 ISP has no WDR either); the
# WDR output mode ids reach the driver, which answers "non-WDR".  DMIC stays
# (the T33 2.0.2.1 libimp exports the same 28 IMP_DMIC_* names); no DVP API.
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output_dir=${T33_OUTPUT_DIR:-"$project_dir/build/t33"}

# Toolchain: TOOLCHAIN_PREFIX, else a Thingino T32 build (output/<profile>/*t32*
# in THINGINO_DIR, or a thingino-out/*t32* build directory next to the repo).
if [ -z "${TOOLCHAIN_PREFIX:-}" ]; then
    for candidate in \
        ${THINGINO_DIR:+"$THINGINO_DIR"/output/*/*t3[23]*/host/bin/mipsel-linux-gcc} \
        "$project_dir"/../thingino-out/*t3[23]*/host/bin/mipsel-linux-gcc \
        "$project_dir"/../../thingino-out/*t3[23]*/host/bin/mipsel-linux-gcc \
        "$project_dir"/../../../thingino-out/*t3[23]*/host/bin/mipsel-linux-gcc
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
base_flags="$base_flags -fPIC -G0 -fno-stack-protector -DPLATFORM_T32 -DPLATFORM_T33"
base_flags="$base_flags -ffunction-sections -fdata-sections"
base_flags="$base_flags -DOPENIMP_DEBUG=${OPENIMP_DEBUG:-0}"
strict_flags="$base_flags -Wall -Wextra -Werror"
repo_includes="-I$project_dir/include -I$project_dir/src"

# Vendor T32 2.2.0 public headers (ingenic-headers T33/2.0.2.1/en, as the T41
# build uses its 1.2.0 headers): the tuning API's prototypes and layouts
if [ -z "${T33_HEADERS:-}" ]; then
    for candidate in \
        ${THINGINO_DIR:+"$THINGINO_DIR"/dl/thingino-raptor-hal/git/ingenic-headers/T33/2.0.2.1/en} \
        ${THINGINO_DIR:+"$THINGINO_DIR"/output/*/*/build/thingino-raptor-hal-*/ingenic-headers/T33/2.0.2.1/en} \
        "$project_dir"/../thingino-out/*/build/thingino-raptor-hal-*/ingenic-headers/T33/2.0.2.1/en \
        "$project_dir"/../../thingino-out/*/build/thingino-raptor-hal-*/ingenic-headers/T33/2.0.2.1/en \
        "$project_dir"/../../../thingino-out/*/build/thingino-raptor-hal-*/ingenic-headers/T33/2.0.2.1/en
    do
        if [ -f "$candidate/imp/imp_isp.h" ]; then
            T33_HEADERS=$candidate
            break
        fi
    done
fi
: "${T33_HEADERS:?set T33_HEADERS to the vendor T32 2.2.0 header root (ingenic-headers T33/2.0.2.1/en)}"
test -f "$T33_HEADERS/imp/imp_isp.h"

objects=
compile()
{
    object=$1
    source=$2
    shift 2
    "$compiler" $strict_flags $repo_includes "$@" \
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
# ISP tuning on /dev/isp-m0 (#296): vendor 2.2.0 headers instead of include/
"$compiler" $strict_flags -I"$T33_HEADERS" -I"$project_dir/src" \
    -c "$project_dir/src/t32/openimp_t32_tuning.c" -o "$output_dir/t32_tuning.o"
objects="$objects $output_dir/t32_tuning.o"
# ISP-drawn OSD (#336): picture regions, ISP lines / covers, same headers
"$compiler" $strict_flags -I"$T33_HEADERS" -I"$project_dir/src" \
    -c "$project_dir/src/t32/openimp_t32_isp_osd.c" -o "$output_dir/t32_isp_osd.o"
objects="$objects $output_dir/t32_isp_osd.o"
# FrameSource on the 2.x framechan ABI, System bind (#290)
compile t32_fs src/t32/openimp_t32_fs.c
# IVDC direct mode (#301): ISP channel 0 -> encoder without frame buffers
compile t32_ivdc src/t32/openimp_t32_ivdc.c
# soc_vpu channel layer (#291) and the JPEG encoder on the JPEG v1.2 engine (#292)
compile t32_vpu src/t32/openimp_t32_vpu.c
compile t32_encoder src/t32/openimp_t32_encoder.c
# IMP_Decoder (#301): the 2.0.0 SDK JPEG decoder on the same engine (codecdir
# JPEGDEC; needs the 2.0.0 kernel's helix decode path)
compile t32_jpegd src/t32/openimp_t32_jpegd.c
compile t32_decoder src/t32/openimp_t32_decoder.c
# rate control (#295): the T32 attributes on the eprc controller (src/eprc,
# T23 revision; HERA keeps the Helix statistics registers)
compile t32_rc src/t32/openimp_t32_rc.c
compile t32_eprc src/eprc/eprc.c -O2 -ffp-contract=off
compile t32_eprc_mbrc src/eprc/eprc_mbrc.c -O2 -ffp-contract=off
# H.264 on HERA (#293): command lists, parameter sets / slice headers, the
# CABAC context tables shared with the T30 encoder
compile t32_hera src/t32/openimp_t32_hera.c
# HERA v1.6 (#391): the T33 H.264 command lists (VMAU at 0x40400, the
# compressed reconstruction area, core emulation prevention)
compile t33_hera src/t32/openimp_t33_hera.c
compile t33_hera_h265 src/t32/openimp_t33_hera_h265.c
compile t32_h264 src/t32/openimp_t32_h264.c
compile t32_h264_cabac src/t30/h264enc/cabac.c
compile t32_h264_common src/t30/h264enc/common.c
# H.265 on HERA (#294): CTB-64 command lists, VPS / SPS / PPS / slice headers
# with the T30 Radix HEVC bit writers and CABAC context init
compile t32_hera_h265 src/t32/openimp_t32_hera_h265.c
compile t32_h265 src/t32/openimp_t32_h265.c
compile t32_hevc_syntax src/t30/radix/t30_radix_hevc.c
compile t32_hevc_cabac src/t30/radix/t30_radix_cabac.c
# IVS (#299): the shared framework and move / base-move algorithms on the
# T41 frame ABI (T32 2.x IMPFrameInfo = T41), fed through a FrameSource sink
compile t32_ivs src/t31/openimp_t31_ivs.c -I"$project_dir/src/t31"
compile t32_ivs_move src/t31/openimp_t31_ivs_move.c -O2 -I"$project_dir/src/t31"
compile t32_ivs_move_v2 src/t31/openimp_ivs_move_v2.c -O2 -I"$project_dir/src/t31"
# IMP_Buf_* pools, MemPoolRequest/Free, Read/WriteReg32 (#299)
compile t32_sys src/t32/openimp_t32_sys.c
# OSD (#297): the shared T23-family IPU OSD in the T32 2.x layout (272-byte
# region attribute, 200-byte jz_ipu_v13 request with the 2-bit OSD fields),
# OSD groups as FrameSource receivers (FS -> OSD -> encoder)
compile t32_osd src/t23/openimp_t23_osd.c -O2
# Audio (#298): AI/AO on the OSS3 /dev/dsp ABI of the T32 2.x kernel, which is
# the T41 one (vendor 2.0.0 drivers/audio/oss3 audio_dsp.h); the T41 paths of
# the shared modules with PLATFORM_T32: P3 AI/AO, DMIC (OSS3 variant), AENC/ADEC,
# EnableAlgo/Hs and the IMPDBG entry points
compile t32_audio src/t40/openimp_p3_audio.c
compile t32_dmic src/t31/openimp_t31_dmic.c
compile t32_audio_ext src/t23/openimp_t23_audio_ext.c
compile t32_audio_codec src/audio/openimp_audio_codec.c
compile t32_audio_enc_dec src/audio/openimp_audio_enc_dec.c
# SU_*: not in libimp on T32.  The T32 SDK keeps them in libsysutils (vendor
# libimp 2.x exports none) and the T32 firmware ships its own libsysutils, so
# libimp does not shadow it (#299: QEMU check in tools/t32/t32_misc_sample.c su)

rm -f "$output_dir/libimp.a"
"$archiver" rcs "$output_dir/libimp.a" $objects

"$compiler" -shared -nostartfiles \
    -Wl,-soname,libimp.so -Wl,--gc-sections \
    -Wl,--version-script="$project_dir/src/t40/libimp.map" \
    -o "$output_dir/libimp.so" $objects \
    -ldl -lpthread -lrt -lm

if readelf -d "$output_dir/libimp.so" |
    grep -q 'Shared library: \[libimp.so'
then
    echo "T33 build has an OEM libimp dependency" >&2
    exit 1
fi

echo "T33 skeleton: $(readelf --dyn-syms --wide "$output_dir/libimp.so" |
    awk '$7 != "UND" && $5 == "GLOBAL" && $8 ~ /^IMP_/' | wc -l) IMP_ exports"
sha256sum "$output_dir/libimp.so" "$output_dir/libimp.a"
readelf -d "$output_dir/libimp.so" | grep -E 'SONAME|NEEDED'
