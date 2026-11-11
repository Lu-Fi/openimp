#!/bin/bash
# T32 trace oracle (#287): boot QEMU -M ingenic-t32 with a T32 ISP module and
# a libimp, run tools/t32/t32_trace_sample under the LD_PRELOAD recorder and
# write the ordered ioctl sequence (tools/t32/t32_ioctl_seq.py).
#
# Reference (vendor libimp 2.2.0 + open-tx-isp driver/t32):
#   QEMU=... KERNEL=... BASE_ROOTFS=... ISP_KO=... T32_BUILD=... \
#     tools/t32/qemu-ioctl-trace.sh h264 5 OUT_DIR
# Later issues pass LIBIMP=build/t32/libimp.so to trace OpenIMP and compare:
#   tools/t32/t32_ioctl_seq.py diff tests/t32/vendor-2.2.0-ioctl-seq.txt OUT_DIR/seq.txt
#
# QEMU        qemu-system-mipsel with -M ingenic-t32 (gtxaspec ingenic 0.7.14+)
# KERNEL      T32 kernel 4.4.94 uImage (thingino qemu_t32nq k44 build)
# BASE_ROOTFS unpacked rootfs of that build: vendor libimp 2.2.0, libalog,
#             sensor_cv8001_t32.ko, /etc/sensor/cv8001-PRJ007-{day,night}.bin
# ISP_KO      tx-isp-t32.ko to test (open-tx-isp driver/t32)
# T32_BUILD   thingino T32 output dir (host/bin/mipsel-linux-gcc, mksquashfs,
#             target/ sysroot with the libimp to link against)
# LIBIMP      optional libimp.so to install instead of the rootfs one
# PORT/MPORT  console / monitor TCP ports (default 3391/3392)
# CPUS        taskset CPU list for QEMU (default 4-15; the host is shared)
set -eu
CODEC=${1:-h264}; STREAMS=${2:-5}; OUT=$(readlink -f "${3:-./t32-trace-out}")
REPO=$(cd "$(dirname "$0")/../.." && pwd)
: "${QEMU:?}" "${KERNEL:?}" "${BASE_ROOTFS:?}" "${ISP_KO:?}" "${T32_BUILD:?}"
PORT=${PORT:-3391}; MPORT=${MPORT:-3392}; CPUS=${CPUS:-4-15}
CC=$T32_BUILD/host/bin/mipsel-linux-gcc; SYSROOT=$T32_BUILD/target
mkdir -p "$OUT"; rm -rf "$OUT/rootfs"; cp -a "$BASE_ROOTFS" "$OUT/rootfs"

# sample + recorder (own code), module and optional libimp under test
"$CC" -std=gnu99 -O1 -Wall -Wextra -Werror -DPLATFORM_T32 -I"$REPO/include" \
    -o "$OUT/rootfs/usr/bin/t32_trace_sample" "$REPO/tools/t32/t32_trace_sample.c" \
    -L"$SYSROOT/usr/lib" -L"$SYSROOT/lib" -Wl,--no-as-needed -limp -lalog -lsysutils \
    -luclibcshim -lpthread -lrt -ldl -lm -Wl,--allow-shlib-undefined
"$CC" -std=gnu99 -O2 -Wall -Wextra -Werror -fPIC -shared \
    -o "$OUT/rootfs/usr/lib/libt32trace.so" "$REPO/tools/t32/t32_ioctl_trace.c" -ldl -lpthread
cp "$ISP_KO" "$OUT/rootfs/lib/modules/4.4.94/ingenic/tx-isp-t32.ko"
[ -n "${LIBIMP:-}" ] && cp "$LIBIMP" "$OUT/rootfs/usr/lib/libimp.so"
md5sum "$ISP_KO" "$OUT/rootfs/usr/lib/libimp.so" > "$OUT/inputs.md5"
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
    -device image-sensor,model=cv8001,bus=i2c0 -no-reboot > "$OUT/qemu.stdout" 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT

# console driver: send a command, read until the end marker (or timeout)
cat > "$OUT/con.py" <<'PY'
import socket, sys, time
port, timeout, cmd = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
for _ in range(100):
    try:
        s = socket.create_connection(("127.0.0.1", port)); break
    except OSError:
        time.sleep(0.3)
s.settimeout(0.5)
s.sendall((cmd + "; echo __DONE__$?\n").encode())
buf, t0 = b"", time.time()
while time.time() - t0 < timeout:
    try:
        d = s.recv(65536)
        if d:
            buf += d
            if b"\n__DONE__" in buf.replace(b"\r", b""):
                time.sleep(0.3); break
    except socket.timeout:
        pass
sys.stdout.write(buf.decode(errors="replace"))
PY
con() { python3 "$OUT/con.py" "$PORT" "$1" "$2"; }

timeout 120 bash -c "until grep -q 'job control' '$OUT/console.log' 2>/dev/null; do sleep 1; done"
con 20 "mount -t proc p /proc; mount -t tmpfs t /tmp; mount -t sysfs s /sys; mkdir -p /dev/shm; mount -t tmpfs shm /dev/shm" > /dev/null
con 120 "modprobe tx_isp_t32 && modprobe sensor_cv8001_t32" > "$OUT/modprobe.txt"
con 900 "cd /tmp; T32_TRACE_LOG=/tmp/t.log T32_TRACE_DUMP=${DUMP:-0} LD_PRELOAD=/usr/lib/libt32trace.so t32_trace_sample $CODEC $STREAMS > /tmp/s.out 2>&1" > /dev/null
con 60 "cat /tmp/s.out" | tr -d '\r' > "$OUT/sample.txt"
con 600 "echo __LOG_BEGIN; cat /tmp/t.log; echo __LOG_END" | tr -d '\r' |
    awk '/^__LOG_BEGIN/{f=1;next}/^__LOG_END/{f=0}f' > "$OUT/raw.log"
con 60 "dmesg" | tr -d '\r' > "$OUT/dmesg.txt"
kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true; trap - EXIT
grep '^T32S' "$OUT/sample.txt" || true
python3 "$REPO/tools/t32/t32_ioctl_seq.py" norm "$OUT/raw.log" \
    --header "$REPO/src/t32/openimp_t32_ioctl.h" > "$OUT/seq.txt"
echo "sequence: $OUT/seq.txt ($(grep -c '^[=~]' "$OUT/seq.txt") lines)"
