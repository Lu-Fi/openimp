#!/bin/bash
# openimp-apitest for T32 in QEMU (#300, planned): boot -M ingenic-t32 with a
# T32 ISP module, install build/t32/openimp-apitest and a libimp, run the
# areas with APITEST_QEMU=1 and copy the report and the --json file out.
#
#   QEMU=... KERNEL=... BASE_ROOTFS=... ISP_KO=... T32_BUILD=... \
#     LIBIMP=build/t32/libimp.so tools/t32/qemu-apitest.sh OUT_DIR [areas]
#
# Same environment as tools/t32/qemu-ioctl-trace.sh (QEMU, KERNEL,
# BASE_ROOTFS, ISP_KO, T32_BUILD, PORT/MPORT, CPUS, MODPROBE).  APITEST is
# the binary (default build/t32/openimp-apitest, tools/apitest/build-device.sh
# PLATFORM=T32); LIBIMP the libimp under test (default: the rootfs one, the
# vendor 2.2.0).  areas default: sys,fs,enc,osd,ivs,audio,isp,su,cases.
# APITEST_ENV: extra environment for the run (e.g. APITEST_CASES=...).
# Output: OUT/apitest.txt (the [A]/[T] lines), OUT/apitest.json, dmesg.txt.
set -eu
OUT=$(readlink -f "${1:?OUT_DIR}"); AREAS=${2:-sys,fs,enc,osd,ivs,audio,isp,su,cases}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
: "${QEMU:?}" "${KERNEL:?}" "${BASE_ROOTFS:?}" "${ISP_KO:?}" "${T32_BUILD:?}"
PORT=${PORT:-3391}; MPORT=${MPORT:-3392}; CPUS=${CPUS:-4-15}
APITEST=${APITEST:-$REPO/build/t32/openimp-apitest}
mkdir -p "$OUT"; rm -rf "$OUT/rootfs"; cp -a "$BASE_ROOTFS" "$OUT/rootfs"
cp "$APITEST" "$OUT/rootfs/usr/bin/openimp-apitest"
cp "$ISP_KO" "$OUT/rootfs/lib/modules/4.4.94/ingenic/tx-isp-t32.ko"
[ -n "${LIBIMP:-}" ] && cp "$LIBIMP" "$OUT/rootfs/usr/lib/libimp.so"
md5sum "$APITEST" "$ISP_KO" "$OUT/rootfs/usr/lib/libimp.so" > "$OUT/inputs.md5"
rm -f "$OUT/rootfs.bin"
"$T32_BUILD/host/bin/mksquashfs" "$OUT/rootfs" "$OUT/rootfs.bin" -comp xz -b 262144 \
    -all-root -noappend -quiet > /dev/null
truncate -s 16M "$OUT/rootfs.bin"

CMDLINE="console=ttyS1,115200n8 mem=64M@0x0 rmem=64M@0x4000000 root=/dev/mtdblock0 rootfstype=squashfs ro init=/bin/sh"
rm -f "$OUT/console.log"
taskset -c "$CPUS" nice -n 19 "$QEMU" -M ingenic-t32 -m 128M -display none \
    -drive file="$OUT/rootfs.bin",format=raw,if=none,id=flash0 -kernel "$KERNEL" -append "$CMDLINE" \
    -serial null -chardev socket,id=con,host=127.0.0.1,port=$PORT,server=on,wait=off,logfile="$OUT/console.log" \
    -serial chardev:con -monitor tcp:127.0.0.1:$MPORT,server=on,wait=off \
    -device image-sensor,model=cv8001,bus=i2c0${SENSOR_OPTS:-} -no-reboot ${QEMU_EXTRA:-} > "$OUT/qemu.stdout" 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT
# the console driver of qemu-ioctl-trace.sh
sed -n "/^cat > \"\$OUT\/con.py\" <<'PY'/,/^PY/p" "$REPO/tools/t32/qemu-ioctl-trace.sh" | sed '1d;$d' > "$OUT/con.py"
con() { python3 "$OUT/con.py" "$PORT" "$1" "$2"; }

timeout 120 bash -c "until grep -q 'job control' '$OUT/console.log' 2>/dev/null; do sleep 1; done"
con 20 "dmesg -n 1; mount -t proc p /proc; mount -t tmpfs t /tmp; mount -t sysfs s /sys; mkdir -p /dev/shm; mount -t tmpfs shm /dev/shm" > /dev/null
con 120 "modprobe tx_isp_t32 && modprobe sensor_cv8001_t32" > "$OUT/modprobe.txt"
for m in ${MODPROBE:-}; do con 120 "modprobe $m" >> "$OUT/modprobe.txt"; done
con 60 "ls -l /dev/ipu /dev/dbox /dev/dsp /dev/soc_vpu /dev/framechan0 2>&1" | tr -d '\r' > "$OUT/nodes.txt"
# the vendor libimp imports IMP_Log_Get_Option/imp_log_fun from libalog.so (vendor apps link -lalog);
# apitest is linked against OpenIMP, so preload libalog when the libimp under test does not define them
PRE=""
if ! grep -aq "OpenIMP" "$OUT/rootfs/usr/lib/libimp.so" && [ -f "$OUT/rootfs/usr/lib/libalog.so" ]; then
    PRE="LD_PRELOAD=/usr/lib/libalog.so"
fi
con 1800 "cd /tmp; APITEST_QEMU=1 $PRE ${APITEST_ENV:-} openimp-apitest --json /tmp/at.json cv8001 0x35 1920 1080 $AREAS > /tmp/at.out 2>&1" > /dev/null
con 120 "echo __OUT_BEGIN; cat /tmp/at.out; echo __OUT_END" | tr -d '\r' |
    awk '/^__OUT_BEGIN/{f=1;next}/^__OUT_END/{f=0}f' > "$OUT/apitest.txt"
con 120 "echo __JS_BEGIN; cat /tmp/at.json; echo; echo __JS_END" | tr -d '\r' |
    awk '/^__JS_BEGIN/{f=1;next}/^__JS_END/{f=0}f' > "$OUT/apitest.json"
con 60 "dmesg" | tr -d '\r' > "$OUT/dmesg.txt"
kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true; trap - EXIT
grep '^\[T\]' "$OUT/apitest.txt" | tail -12 || true
echo "oops: $(grep -c -i 'oops\|unhandled\|kernel bug' "$OUT/dmesg.txt" || true)"
