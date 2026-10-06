#!/bin/bash
# build apitest_<soc> for every SoC (cross toolchains + libimp from thingino-out)
HDR=/mnt/NVMe/git/thingino-ciao-openstack/dl/thingino-raptor-hal/git/ingenic-headers
OUT=/mnt/NVMe/git/thingino-out
D=$(cd "$(dirname "$0")" && pwd)
SRC="$D/apitest.c $D/at_sys.c $D/at_enc.c $D/at_osd_ivs.c $D/at_audio.c $D/at_isp.c $D/at_su.c"
build() { # soc headerdir camdir
  local soc=$1 inc=$2 cam=$OUT/$3
  local cc=$cam/host/bin/mipsel-linux-gcc
  $cc -O2 -Wall -Wno-unused-function -Wno-address -Wno-format-zero-length -Wno-dangling-else -DPLATFORM_$soc -I"$D" -I"$HDR/$inc" -I"$HDR/$inc/imp" -o "$D/apitest_$(echo $soc | tr A-Z a-z)" $SRC \
      -L"$cam/target/usr/lib" -limp -lm -ldl -lpthread -lrt -Wl,-rpath-link,"$cam/target/usr/lib" \
      && echo "OK $soc" || echo "FAIL $soc"
}
only=$1
for e in "T10 T10/3.12.0/zh secuplug_sp1_t10l_jxh42_mt7601-192.168.10.31" \
         "T20 T20/3.12.0/zh wyze_campan1_t20x_jxf22_rtl8189etv-192.168.10.163" \
         "T21 T21/1.0.33/zh victure_pc420_t21n_jxf23_eth+rtl8188ftv-192.168.10.27" \
         "T23 T23/1.3.0/zh jooan_a6m_t23n_sc1a4t_atbm6012bx-192.168.10.30" \
         "T31 T31/1.1.6/en wuuk_y0510_t31x_sc4336p_ssv6158-192.168.10.21" \
         "T41 T41/1.2.6/zh vanhua_t55a_t41lq_gc5603_eth-192.168.178.179"; do
  set -- $e
  [ -z "$only" -o "$only" = "$1" ] && build "$1" "$2" "$3"
done
