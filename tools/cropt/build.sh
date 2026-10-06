#!/bin/bash
# build cropt_t23 (T23 toolchain + libimp from thingino-out, vendor headers)
HDR=/mnt/NVMe/git/thingino-ciao-openstack/dl/thingino-raptor-hal/git/ingenic-headers/T23/1.3.0/zh
CAM=/mnt/NVMe/git/thingino-out/jooan_a6m_t23n_sc1a4t_atbm6012bx-192.168.10.30
D=$(cd "$(dirname "$0")" && pwd)
"$CAM/host/bin/mipsel-linux-gcc" -O2 -Wall -DPLATFORM_T23 -I"$HDR" -I"$HDR/imp" -o "$D/cropt_t23" "$D/cropt.c" \
  -L"$CAM/target/usr/lib" -limp -lm -ldl -lpthread -lrt -Wl,--export-dynamic -Wl,-rpath-link,"$CAM/target/usr/lib" && echo OK
