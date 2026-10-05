#!/bin/bash
# build imgfx_<soc> for every SoC (cross toolchains + libimp from thingino-out)
HDR=/mnt/NVMe/git/thingino-ciao-openstack/dl/thingino-raptor-hal/git/ingenic-headers
OUT=/mnt/NVMe/git/thingino-out
D=$(cd "$(dirname "$0")" && pwd)
build() { # soc headerdir camdir
  local soc=$1 inc=$2 cam=$OUT/$3
  local cc=$cam/host/bin/mipsel-linux-gcc
  $cc -O2 -Wall -Wno-unused-function -Wno-address -DPLATFORM_$soc -I"$HDR/$inc" -o "$D/imgfx_$(echo $soc | tr A-Z a-z)" "$D/imgfx.c" \
      -L"$cam/target/usr/lib" -limp -lm -ldl -lpthread -lrt -Wl,-rpath-link,"$cam/target/usr/lib" \
      && echo "OK $soc" || echo "FAIL $soc"
}
build T10 T10/3.12.0/zh secuplug_sp1_t10l_jxh42_mt7601-192.168.10.31
build T20 T20/3.12.0/zh wyze_campan1_t20x_jxf22_rtl8189etv-192.168.10.163
build T21 T21/1.0.33/zh "victure_pc420_t21n_jxf23_eth+rtl8188ftv-192.168.10.27"
build T23 T23/1.3.0/zh jooan_a6m_t23n_sc1a4t_atbm6012bx-192.168.10.30
build T31 T31/1.1.6/en wuuk_y0510_t31x_sc4336p_ssv6158-192.168.10.21
build T41 T41/1.2.6/zh vanhua_t55a_t41lq_gc5603_eth-192.168.178.179
