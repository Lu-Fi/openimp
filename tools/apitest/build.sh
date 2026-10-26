#!/bin/bash
# Build apitest_<soc> and imgfx_<soc> (tools/imgfx) for T10 T20 T21 T23 T31 T40 T41.
# Usage: build.sh [SOC...]   (default: all)   OUT=<dir> for the binaries (default tools/apitest/build)
# The binaries link dynamically against the libimp of the camera they run on (the flashed one); the build
# only needs a toolchain and a libimp.so to link against: taken from a thingino-out tree of the SoC.
# Vendor headers: timps/include (ingenic-headers copies). T40 = T41 API family: -DPLATFORM_T41 -DAPITEST_T40.
HDR=${HDR:-/mnt/NVMe/git/timps/include}
TO=${TO:-/mnt/NVMe/git/thingino-out}
GEN=${GEN:-agg46}
D=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$D/build}
mkdir -p "$OUT"
SRC="$D/apitest.c $D/at_sys.c $D/at_enc.c $D/at_osd_ivs.c $D/at_audio.c $D/at_isp.c $D/at_isp_rt.c $D/at_su.c"
build() { # soc headerdir camdir defs
  local soc=$1 inc=$HDR/$2 cam=$(ls -d $TO/$3-$GEN-* 2>/dev/null | head -1) defs=$4
  local lc=$(echo $soc | tr A-Z a-z)
  [ -d "$cam" ] || { echo "FAIL $soc: no thingino-out tree $3-$GEN-*"; return; }
  local cc=$cam/host/bin/mipsel-linux-gcc
  local l="-L$cam/target/usr/lib -limp -lm -ldl -lpthread -lrt -Wl,-rpath-link,$cam/target/usr/lib"
  local f="-O2 -Wall -Wno-unused-function -Wno-address -Wno-format-zero-length -Wno-dangling-else $defs -I$inc -I$inc/imp"
  nice $cc $f -I"$D" -o "$OUT/apitest_$lc" $SRC $l && echo "OK apitest_$lc" || echo "FAIL apitest_$lc"
  [ "$soc" = T40 ] && { nice $cc -O2 -Wall -Wno-unused-function -I$inc -I$inc/imp -I"$D/../../include" -o "$OUT/t40_devtest" "$D/../../tests/t40/devtest/t40_devtest.c" $l -lpthread && echo "OK t40_devtest" || echo "FAIL t40_devtest"; return; }
  nice $cc $f -o "$OUT/imgfx_$lc" "$D/../imgfx/imgfx.c" $l && echo "OK imgfx_$lc" || echo "FAIL imgfx_$lc"
}
ARGS=("$@")
w() { [ ${#ARGS[@]} -eq 0 ] && return 0; for a in "${ARGS[@]}"; do [ "$a" = "$1" ] && return 0; done; return 1; }
w T10 && build T10 T10/3.12.0/zh secuplug_sp1_t10l_jxh42_mt7601 -DPLATFORM_T10
w T20 && build T20 T20/3.12.0/zh wyze_cam2_t20x_jxf23_rtl8189ftv -DPLATFORM_T20
w T21 && build T21 T21/1.0.33/zh "victure_pc420_t21n_jxf23_eth+rtl8188ftv" -DPLATFORM_T21
w T23 && build T23 T23/1.3.0/zh jooan_a6m_t23n_sc1a4t_atbm6012bx -DPLATFORM_T23
w T31 && build T31 T31/1.1.6/en wuuk_y0510_t31x_sc4336p_ssv6158 -DPLATFORM_T31
w T40 && build T40 T40/1.3.1/en eufy_t8416_t40xp_sc830_aic8800d40 "-DPLATFORM_T41 -DAPITEST_T40"
w T41 && build T41 T41/1.2.6/zh vanhua_t55a_t41lq_gc5603_eth "-DPLATFORM_T41"
