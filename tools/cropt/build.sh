#!/bin/bash
# build cropt_<soc>: build.sh [T23|T10]  (toolchain + libimp from thingino-out, vendor headers)
HDR=/mnt/NVMe/git/thingino-ciao-openstack/dl/thingino-raptor-hal/git/ingenic-headers
OUT=/mnt/NVMe/git/thingino-out
D=$(cd "$(dirname "$0")" && pwd)
build() { # SOC headerdir camdir
  local cam=$OUT/$3
  "$cam/host/bin/mipsel-linux-gcc" -O2 -Wall -DPLATFORM_$1 -I"$HDR/$2" -I"$HDR/$2/imp" -o "$D/cropt_$(echo $1 | tr A-Z a-z)" "$D/cropt.c" \
    -L"$cam/target/usr/lib" -limp -lm -ldl -lpthread -lrt -Wl,--export-dynamic -Wl,-rpath-link,"$cam/target/usr/lib" && echo "OK $1" || echo "FAIL $1"
}
case "${1:-all}" in T23|all) build T23 T23/1.3.0/zh jooan_a6m_t23n_sc1a4t_atbm6012bx-192.168.10.30;; esac
case "${1:-all}" in T10|all) build T10 T10/3.12.0/zh secuplug_sp1_t10l_jxh42_mt7601-192.168.10.31;; esac
