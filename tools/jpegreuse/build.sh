#!/bin/bash
# jpegreuse build for T21: build.sh <dir with libimp.so> <out> <toolchain target dir>
H=x
nice $3/host/bin/mipsel-linux-gcc -O2 -Wall -DPLATFORM_T21 -I$(dirname $0)/../../include -I$(dirname $0)/../../src -o $2 $(dirname $0)/jpegreuse.c -L$1 -limp -lm -ldl -lpthread -lrt -Wl,--export-dynamic -Wl,--allow-shlib-undefined -Wl,-rpath-link,$3/target/usr/lib
