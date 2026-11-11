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
# SNAP=1      after the run, save the frame the sample announces ("T32F snap
#             pa=... size=...", modes none/bind) with the QEMU monitor's
#             pmemsave to OUT/snap.nv12 (tools/t32/nv12png.py makes a PNG;
#             pictures stay local)
# SAMPLE_ENV  extra environment for the sample, e.g. T32_NO_PROC=1 (skip the
#             /proc/jz/isp/isp-m0 reads; the vendor tx-isp-t32.ko oopses there)
# STREAMS_OUT=1  the sample writes every encoder stream to /tmp/streams.bin
#             (T32_STREAM_DUMP); the sample copies it to physical 0x7800000
#             (top of rmem, after the teardown) and pmemsave writes it to
#             OUT/streams.bin; tools/t32/t32_streams.py splits it (local only)
# SAMPLE_SRC  sample source to build instead of tools/t32/t32_trace_sample.c
#             (e.g. tools/t32/t32_misc_sample.c, #299/#298); installed under
#             the same name
# SENSOR_OPTS extra image-sensor properties, e.g. ",scene=/path/clip.y4m"
#             (a moving scene for IVS, qemu/README-scene)
# EXTRA_LIBS  libraries copied into the guest /usr/lib before the run (e.g. the
#             vendor libsysutils 2.x for an SU_* reference run)
# EXTRA_FILES data files copied into the guest /usr/share/t32 (e.g. the JPEGs
#             tools/t32/t32_dec_sample.c decodes, #301)
# MODPROBE    extra modules loaded after the ISP and sensor (e.g. "audio" for
#             the OSS3 /dev/dsp driver, #298); ":" separates module
#             parameters: "audio:dmic_enable=1" also registers the DMIC
#             route (#330: without it the DMIC ioctls oops in the driver)
# ISP_ARGS    module parameters for tx_isp_t32, e.g. "direct_mode=1" (IVDC
#             direct mode, #301; the vendor module has it)
# QEMU_EXTRA  extra QEMU arguments, e.g. "-trace ingenic_isp_write,file=OUT/isp.trace"
#             (driver register writes for the vendor-vs-OpenIMP comparison, #289)
set -eu
CODEC=${1:-h264}; STREAMS=${2:-5}; OUT=$(readlink -f "${3:-./t32-trace-out}")
REPO=$(cd "$(dirname "$0")/../.." && pwd)
: "${QEMU:?}" "${KERNEL:?}" "${BASE_ROOTFS:?}" "${ISP_KO:?}" "${T32_BUILD:?}"
PORT=${PORT:-3391}; MPORT=${MPORT:-3392}; CPUS=${CPUS:-4-15}
CC=$T32_BUILD/host/bin/mipsel-linux-gcc; SYSROOT=$T32_BUILD/target
mkdir -p "$OUT"; rm -rf "$OUT/rootfs"; cp -a "$BASE_ROOTFS" "$OUT/rootfs"

# sample + recorder (own code), module and optional libimp under test
"$CC" -std=gnu99 -O1 -Wall -Wextra -Werror -DPLATFORM_T32 -I"$REPO/include" -I"$REPO/src" \
    -o "$OUT/rootfs/usr/bin/t32_trace_sample" "$REPO/${SAMPLE_SRC:-tools/t32/t32_trace_sample.c}" \
    -L"$SYSROOT/usr/lib" -L"$SYSROOT/lib" -Wl,--no-as-needed -limp -lalog -lsysutils \
    -luclibcshim -lpthread -lrt -ldl -lm -Wl,--allow-shlib-undefined
"$CC" -std=gnu99 -O2 -Wall -Wextra -Werror -fPIC -shared \
    -o "$OUT/rootfs/usr/lib/libt32trace.so" "$REPO/tools/t32/t32_ioctl_trace.c" -ldl -lpthread
cp "$ISP_KO" "$OUT/rootfs/lib/modules/4.4.94/ingenic/tx-isp-t32.ko"
[ -n "${LIBIMP:-}" ] && cp "$LIBIMP" "$OUT/rootfs/usr/lib/libimp.so"
for l in ${EXTRA_LIBS:-}; do cp "$l" "$OUT/rootfs/usr/lib/"; done
[ -n "${EXTRA_FILES:-}" ] && mkdir -p "$OUT/rootfs/usr/share/t32"
for l in ${EXTRA_FILES:-}; do cp "$l" "$OUT/rootfs/usr/share/t32/"; done
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
    -device image-sensor,model=cv8001,bus=i2c0${SENSOR_OPTS:-} -no-reboot ${QEMU_EXTRA:-} > "$OUT/qemu.stdout" 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT

# console driver: send a command, read until the end marker (or timeout)
cat > "$OUT/con.py" <<'PY'
import re, socket, sys, time
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
            # the marker with its exit code (a wrapped command echo also
            # puts "__DONE__$?" at a line start)
            if re.search(rb"\n__DONE__[0-9]", buf.replace(b"\r", b"")):
                time.sleep(0.3); break
    except socket.timeout:
        pass
sys.stdout.write(buf.decode(errors="replace"))
PY
con() { python3 "$OUT/con.py" "$PORT" "$1" "$2"; }

timeout 120 bash -c "until grep -q 'job control' '$OUT/console.log' 2>/dev/null; do sleep 1; done"
con 20 "dmesg -n 1; mount -t proc p /proc; mount -t tmpfs t /tmp; mount -t sysfs s /sys; mkdir -p /dev/shm; mount -t tmpfs shm /dev/shm" > /dev/null
con 120 "modprobe tx_isp_t32 ${ISP_ARGS:-} && modprobe sensor_cv8001_t32" > "$OUT/modprobe.txt"
for m in ${MODPROBE:-}; do con 120 "modprobe ${m//:/ }; ls -l /dev/dsp* 2>&1" >> "$OUT/modprobe.txt"; done
SDUMP=; [ "${STREAMS_OUT:-0}" = 1 ] && SDUMP=T32_STREAM_DUMP=/tmp/streams.bin
con 900 "cd /tmp; ${SAMPLE_ENV:-} $SDUMP T32_TRACE_LOG=/tmp/t.log T32_TRACE_DUMP=${DUMP:-0} LD_PRELOAD=/usr/lib/libt32trace.so t32_trace_sample $CODEC $STREAMS > /tmp/s.out 2>&1" > /dev/null
con 60 "cat /tmp/s.out" | tr -d '\r' > "$OUT/sample.txt"
con 600 "echo __LOG_BEGIN; cat /tmp/t.log; echo __LOG_END" | tr -d '\r' |
    awk '/^__LOG_BEGIN/{f=1;next}/^__LOG_END/{f=0}f' > "$OUT/raw.log"
con 60 "dmesg" | tr -d '\r' > "$OUT/dmesg.txt"
if [ "${SNAP:-0}" = 1 ] && snap=$(grep -m1 '^T32F snap ' "$OUT/sample.txt"); then
    pa=$(echo "$snap" | sed 's/.*pa=\(0x[0-9a-f]*\).*/\1/'); sz=$(echo "$snap" | sed 's/.*size=\([0-9]*\).*/\1/')
    rm -f "$OUT/snap.nv12"
    python3 - "$MPORT" "pmemsave $pa $sz \"$OUT/snap.nv12\"" <<'PY'
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall((sys.argv[2] + "\n").encode()); time.sleep(2); s.close()
PY
    echo "$snap" > "$OUT/snap.txt"
fi
if [ "${STREAMS_OUT:-0}" = 1 ]; then
    ssz=$(con 30 "wc -c < /tmp/streams.bin" | tr -d '\r' | grep -m1 '^[0-9][0-9]*$' || echo 0)
    if [ "$ssz" -gt 0 ]; then
        rm -f "$OUT/streams.bin"
        python3 - "$MPORT" "pmemsave 0x7800000 $ssz \"$OUT/streams.bin\"" <<'PY'
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall((sys.argv[2] + "\n").encode()); time.sleep(3); s.close()
PY
    fi
fi
kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true; trap - EXIT
grep '^T32S' "$OUT/sample.txt" || true
python3 "$REPO/tools/t32/t32_ioctl_seq.py" norm "$OUT/raw.log" \
    --header "$REPO/src/t32/openimp_t32_ioctl.h" > "$OUT/seq.txt"
echo "sequence: $OUT/seq.txt ($(grep -c '^[=~]' "$OUT/seq.txt") lines)"
