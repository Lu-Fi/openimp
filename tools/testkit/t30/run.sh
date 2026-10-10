#!/bin/sh
# OpenIMP T30 test kit - runs ON the camera (busybox sh), no flashing.
#
#   sh run.sh            full run
#   DRYRUN=1 sh run.sh   logic check on a PC (no camera, no root needed)
#
# Environment overrides (all optional):
#   SENSOR=sc4236 I2C=0x30 W=1920 H=1080   if auto-detection fails
#   AREAS=sys,fs,enc,osd,ivs,isp           apitest areas (audio/su are off by default)
#   STAGE_TIMEOUT=600 ENC_SECONDS=8        per-stage watchdog (s), encode length (s)
#   FORCE=1                                run even if the SoC is not detected as T30
#   KEEP_STREAM=1                          keep the encoded .h264 in /tmp (never packed)
#
# What it never does: write to /usr/lib or any flash path, change a config file,
# play audio, or pack config files / credentials / pictures.  The streamer that
# was running is stopped and ALWAYS started again at the end (trap).

KIT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TS=$(date +%Y%m%d-%H%M%S)
WORK=${WORK:-/tmp/t30kit-run-$TS}
OUT=$WORK/result
TAR=${TAR_OUT:-/tmp/t30-testkit-result-$TS.tar.gz}
STAGE_TIMEOUT=${STAGE_TIMEOUT:-600}
ENC_SECONDS=${ENC_SECONDS:-8}
AREAS=${AREAS:-sys,fs,enc,osd,ivs,isp}
DRYRUN=${DRYRUN:-0}

STREAMER_SCRIPT=""
STREAMER_NAME=""
STREAMER_STOPPED=0
HUNG=0
SUMMARY=""

say() { printf '%s\n' "$*"; }
note() { say "[t30kit] $*"; printf '%s\n' "$*" >>"$OUT/summary.txt" 2>/dev/null; }
kmsg() { [ "$DRYRUN" = 1 ] || { printf 't30kit: %s\n' "$*" >/dev/kmsg; } 2>/dev/null; }

# ---------------------------------------------------------------- cleanup
restart_streamer() {
    [ "$STREAMER_STOPPED" = 1 ] || return 0
    STREAMER_STOPPED=0
    if [ "$DRYRUN" = 1 ]; then
        say "[dry] would run: $STREAMER_SCRIPT start"
        return 0
    fi
    say "[t30kit] restarting streamer: $STREAMER_NAME"
    "$STREAMER_SCRIPT" start >"$OUT/streamer-start.log" 2>&1
    sleep 3
    if find_streamer_proc; then
        note "streamer $STREAMER_NAME is running again"
    else
        note "WARNING: streamer $STREAMER_NAME is not running after start; power-cycle the camera"
    fi
}

cleanup() {
    rc=$?
    trap '' INT TERM HUP
    [ -n "$spid" ] && kill -9 "$spid" 2>/dev/null
    restart_streamer
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

# ---------------------------------------------------------------- helpers
# print comm names of all processes (names only: command lines may carry credentials)
proc_names() {
    for d in /proc/[0-9]*; do
        [ -r "$d/comm" ] && cat "$d/comm"
    done 2>/dev/null
}

find_streamer_proc() {
    [ "$DRYRUN" = 1 ] && return 0
    proc_names | grep -E -q '^(prudynt|timps|rvd|rsd|rmd|rac|raptor)'
}

# first matching init script of a known streamer
find_streamer_script() {
    for pat in prudynt timps raptor rvd; do
        for f in /etc/init.d/S*"$pat"*; do
            [ -x "$f" ] || continue
            case "$f" in *webui*|*motion*|*onvif*|*config*) continue ;; esac
            STREAMER_SCRIPT=$f
            STREAMER_NAME=$pat
            return 0
        done
    done
    return 1
}

# stage NAME TIMEOUT CMD...   runs CMD with a watchdog, log to $OUT/NAME.log
# result in STAGE_RC / STAGE_STATE (OK, FAIL, CRASH, HANG)
stage() {
    sname=$1; stmo=$2; shift 2
    slog=$OUT/$sname.log
    say "[t30kit] stage $sname (timeout ${stmo}s) ..."
    kmsg "stage $sname start"
    s0=$(date +%s)
    "$@" >"$slog" 2>&1 &
    spid=$!
    i=0
    STAGE_STATE=OK
    while kill -0 "$spid" 2>/dev/null; do
        sleep 1
        i=$((i + 1))
        if [ "$i" -ge "$stmo" ]; then
            st=$(sed -n 's/^State:[[:space:]]*\(.\).*/\1/p' "/proc/$spid/status" 2>/dev/null)
            [ -r "/proc/$spid/wchan" ] && wch=$(cat "/proc/$spid/wchan" 2>/dev/null) || wch=
            kill -9 "$spid" 2>/dev/null
            sleep 2
            if kill -0 "$spid" 2>/dev/null; then
                STAGE_STATE=HANG
                note "stage $sname: HANG after ${stmo}s, process state '$st' wchan '$wch' is unkillable (kernel driver stuck). The camera probably needs a power cycle."
            else
                STAGE_STATE=TIMEOUT
                note "stage $sname: TIMEOUT after ${stmo}s (state '$st' wchan '$wch'), process killed"
            fi
            break
        fi
    done
    if [ "$STAGE_STATE" = HANG ]; then
        STAGE_RC=-1
        HUNG=1
    else
        wait "$spid"
        STAGE_RC=$?
        if [ "$STAGE_STATE" = TIMEOUT ]; then :
        elif [ "$STAGE_RC" -gt 128 ]; then STAGE_STATE=CRASH
        elif [ "$STAGE_RC" -ne 0 ]; then STAGE_STATE=FAIL
        fi
    fi
    s1=$(date +%s)
    kmsg "stage $sname end $STAGE_STATE"
    note "stage $sname: $STAGE_STATE (exit $STAGE_RC, $((s1 - s0)) s)"
    SUMMARY="$SUMMARY
$sname=$STAGE_STATE"
    [ "$STAGE_STATE" = HANG ] && return 1
    [ "$STAGE_STATE" = TIMEOUT ] && HUNG=1
    return 0
}

redact() { # stdin -> stdout: MAC and IPv4 addresses
    sed -e 's/\([0-9A-Fa-f]\{2\}:\)\{5\}[0-9A-Fa-f]\{2\}/xx:xx:xx:xx:xx:xx/g' \
        -e 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/x.x.x.x/g'
}

getval() { # getval "KEY" from isp-m0 text in $ISPM0 -> value after ':'
    printf '%s\n' "$ISPM0" | sed -n "s/^[[:space:]]*$1[[:space:]]*:[[:space:]]*//p" | head -n 1 | tr -d '\r'
}

sensor_i2c_default() {
    case "$1" in
        sc1235|sc1245|sc1245a|sc2135|sc2230|sc2232|sc2232h|sc2235|sc2310|sc2335|sc4236|sc4335|sc5235|jxh62|jxh65|mis2003|fuxsc1020) echo 0x30 ;;
        jxf22|jxf23|jxf23s|jxf28|jxf37|jxh63|jxk03) echo 0x40 ;;
        gc2023|gc2033|gc2053) echo 0x37 ;;
        imx291|imx307|imx323|imx335|imx385) echo 0x1a ;;
        imx327) echo 0x36 ;;
        ov2718|ov2732|ov4689|ov5648|os05a10) echo 0x36 ;;
        ov2735|ov2735b|os02b10|sp140a|sp2305) echo 0x3c ;;
        ps5250|ps5260|ps5270|ps5280) echo 0x48 ;;
        bg0806) echo 0x32 ;;
        *) echo "" ;;
    esac
}

# ---------------------------------------------------------------- start
mkdir -p "$OUT" || { say "cannot create $OUT"; exit 2; }
: >"$OUT/summary.txt"
note "OpenIMP T30 test kit, $TS, kit dir $KIT, DRYRUN=$DRYRUN"
note "This run does not flash anything and does not modify /usr/lib or any config file."

if [ "$DRYRUN" = 1 ]; then
    # fake binaries so the stage logic can be exercised on a PC
    FAKE=$WORK/fake
    mkdir -p "$FAKE/lib"
    cat >"$FAKE/apitest_t30" <<'F'
#!/bin/sh
[ -n "$DRY_HANG" ] && [ "$LD_LIBRARY_PATH" != "" ] && { sleep 600; exit 0; }
echo "[T] apitest T30 sensor $1 $3x$4 areas $5"
echo "[A] IMP_System_Init ret=0 PASS"
echo "[T] summary T30: PASS 1 FAIL 0 N/A 0 SKIP 0, OVERWRITE 0"
F
    cat >"$FAKE/t30_encode" <<'F'
#!/bin/sh
echo "[R] t30_encode fake"; head -c 4096 /dev/zero >"$6"
F
    chmod +x "$FAKE/apitest_t30" "$FAKE/t30_encode"
    : >"$FAKE/lib/libimp.so"
    APITEST=$FAKE/apitest_t30; ENCODE=$FAKE/t30_encode; OIMP_LIB=$FAKE/lib
    SENSOR=${SENSOR:-sc4236}; W=${W:-1920}; H=${H:-1080}
    STREAMER_SCRIPT=$FAKE/S95fake; STREAMER_NAME=fake
    printf '#!/bin/sh\necho "[dry] fake streamer $1"\n' >"$STREAMER_SCRIPT"; chmod +x "$STREAMER_SCRIPT"
    SOC=t30x
else
    APITEST=$KIT/bin/apitest_t30; ENCODE=$KIT/bin/t30_encode; OIMP_LIB=$KIT/lib
fi
for f in "$APITEST" "$ENCODE" "$OIMP_LIB/libimp.so"; do
    [ -e "$f" ] || { note "missing kit file: $f"; exit 2; }
done
chmod +x "$APITEST" "$ENCODE" 2>/dev/null

# ---------------------------------------------------------------- detect
if [ "$DRYRUN" != 1 ]; then
    SOC=$( (soc -f) 2>/dev/null | tr 'A-Z' 'a-z' | tr -d '\n')
    [ -n "$SOC" ] || SOC=$(sed -n 's/^system type[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -n 1 | tr 'A-Z' 'a-z')
fi
note "SoC: ${SOC:-unknown}"
case "$SOC" in
    *t30*) ;;
    *) if [ "$FORCE" != 1 ]; then
           note "This does not look like a T30 (set FORCE=1 to run anyway). Nothing was touched."
           exit 3
       fi ;;
esac

ISPM0=$(cat /proc/jz/isp/isp-m0 2>/dev/null)
if [ "$DRYRUN" != 1 ]; then
    [ -n "$SENSOR" ] || SENSOR=$(cat /proc/jz/sensor/sensor0/name 2>/dev/null || cat /proc/jz/sensor/name 2>/dev/null)
    [ -n "$SENSOR" ] || SENSOR=$(getval "SENSOR NAME")
    [ -n "$W" ] || W=$(getval "SENSOR OUTPUT WIDTH")
    [ -n "$H" ] || H=$(getval "SENSOR OUTPUT HEIGHT")
    [ -n "$W" ] || W=$(cat /proc/jz/sensor/sensor0/width 2>/dev/null || cat /proc/jz/sensor/width 2>/dev/null)
    [ -n "$H" ] || H=$(cat /proc/jz/sensor/sensor0/height 2>/dev/null || cat /proc/jz/sensor/height 2>/dev/null)
fi
SENSOR=$(printf '%s' "$SENSOR" | tr 'A-Z' 'a-z' | tr -cd 'a-z0-9')
if [ -z "$I2C" ]; then
    I2C=$(cat /proc/jz/sensor/sensor0/i2c_addr 2>/dev/null || cat /proc/jz/sensor/i2c_addr 2>/dev/null)
    case "$I2C" in 0x*) ;; [0-9]*) I2C=$(printf '0x%x' "$I2C") ;; *) I2C= ;; esac
    [ -n "$I2C" ] || I2C=$(sensor_i2c_default "$SENSOR")
fi
note "sensor: ${SENSOR:-?} i2c ${I2C:-?} size ${W:-?}x${H:-?}"
if [ -z "$SENSOR" ] || [ -z "$I2C" ] || [ -z "$W" ] || [ -z "$H" ]; then
    note "Could not detect sensor/i2c/size. Re-run as: SENSOR=sc4236 I2C=0x30 W=1920 H=1080 sh run.sh"
    exit 3
fi

VLIB=/usr/lib/libimp.so
[ -e "$VLIB" ] || VLIB=/lib/libimp.so
{
    echo "uname: $(uname -srm)"
    echo "vendor libimp: $VLIB"
    ls -l "$VLIB" 2>&1 | sed 's/^.*root *//'
    md5sum "$VLIB" 2>&1
    # version string(s) baked into the vendor library
    grep -a -o 'IMP-[0-9][0-9A-Za-z._-]*\|libimp[ _]version[^"]\{0,30\}' "$VLIB" 2>/dev/null | sort -u | head -n 5
    echo "kit libimp (OpenIMP):"
    ls -l "$OIMP_LIB/libimp.so"
    md5sum "$OIMP_LIB/libimp.so"
    echo "kernel modules:"
    lsmod 2>/dev/null
    echo "version: $(cat /proc/version 2>/dev/null | redact)"
    echo "cmdline (mem/rmem only):"
    tr ' ' '\n' </proc/cmdline 2>/dev/null | grep -E '^(mem|rmem|isp_mem|nmem)' 
    echo "meminfo:"
    head -n 4 /proc/meminfo 2>/dev/null
    echo "sensor files in /proc/jz/sensor:"
    for f in /proc/jz/sensor/* /proc/jz/sensor/sensor0/*; do
        [ -f "$f" ] && printf '%s: %s\n' "$f" "$(cat "$f" 2>/dev/null | head -n 3 | tr '\n' ' ')"
    done
    echo "/etc/sensor listing:"
    ls /etc/sensor 2>/dev/null
} >"$OUT/system-info.txt" 2>&1
[ -n "$ISPM0" ] && printf '%s\n' "$ISPM0" >"$OUT/isp-m0-before.txt"
for f in /proc/jz/isp/isp-fs /proc/jz/isp/isp_info /proc/jz/sinfo/info /proc/jz/clock/clocks; do
    [ -r "$f" ] && { echo "== $f"; cat "$f"; } >>"$OUT/proc-jz-before.txt" 2>/dev/null
done
dmesg >"$OUT/dmesg-before.txt" 2>&1
note "vendor libimp: $(grep -a -o 'IMP-[0-9][0-9A-Za-z._-]*' "$VLIB" 2>/dev/null | head -n 1) ($VLIB)"

# ---------------------------------------------------------------- streamer
if [ "$DRYRUN" != 1 ]; then
    if find_streamer_proc; then
        if ! find_streamer_script; then
            note "A streamer is running but no init script was found, so it could not be restarted safely. Nothing was touched."
            exit 3
        fi
        note "stopping streamer via $STREAMER_SCRIPT (will be restarted at the end)"
        STREAMER_STOPPED=1
        "$STREAMER_SCRIPT" stop >"$OUT/streamer-stop.log" 2>&1
        sleep 3
        # anything still holding libimp is a leftover streamer process: report, TERM, KILL
        left=""
        for d in /proc/[0-9]*; do
            grep -q 'libimp' "$d/maps" 2>/dev/null && left="$left $(cat "$d/comm" 2>/dev/null)"
        done
        if [ -n "$left" ]; then
            note "processes still using libimp after stop:$left - terminating them"
            for d in /proc/[0-9]*; do
                if grep -q 'libimp' "$d/maps" 2>/dev/null; then kill "${d#/proc/}" 2>/dev/null; fi
            done
            sleep 3
            for d in /proc/[0-9]*; do
                if grep -q 'libimp' "$d/maps" 2>/dev/null; then kill -9 "${d#/proc/}" 2>/dev/null; fi
            done
            sleep 1
        fi
    else
        note "no known streamer (prudynt/timps/raptor) is running"
    fi
else
    note "DRYRUN: pretend streamer $STREAMER_NAME stopped"
    STREAMER_STOPPED=1
fi

# ---------------------------------------------------------------- stages
RUN_AT="$APITEST $SENSOR $I2C $W $H $AREAS"
if stage apitest-vendor "$STAGE_TIMEOUT" env LD_LIBRARY_PATH= $RUN_AT; then
    if [ "$HUNG" = 0 ]; then
        sleep 3
        stage apitest-openimp "$STAGE_TIMEOUT" env LD_LIBRARY_PATH="$OIMP_LIB" $RUN_AT
    else
        note "skipping OpenIMP stages: the vendor baseline hung"
    fi
fi
if [ "$HUNG" = 0 ]; then
    sleep 3
    ENCFILE=$WORK/openimp-t30.h264
    stage encode-openimp "$STAGE_TIMEOUT" env LD_LIBRARY_PATH="$OIMP_LIB" "$ENCODE" "$SENSOR" "$I2C" "$W" "$H" "$ENC_SECONDS" "$ENCFILE"
    if [ -f "$ENCFILE" ]; then
        # structure only (NAL types), the picture itself is never packed
        {
            echo "encoded file size: $(wc -c <"$ENCFILE") bytes (file stays on the camera in /tmp, is not packed)"
            echo "first 48 bytes (SPS/PPS header):"
            head -c 48 "$ENCFILE" | od -An -tx1
        } >"$OUT/encode-stream-info.txt" 2>&1
        [ "$KEEP_STREAM" = 1 ] && note "kept $ENCFILE (not packed; delete it when done)" || rm -f "$ENCFILE"
    fi
else
    note "skipping encode stage: an earlier stage hung"
fi

# ---------------------------------------------------------------- collect
kmsg "collect"
dmesg 2>&1 | redact >"$OUT/dmesg-after.txt"
redact <"$OUT/dmesg-before.txt" >"$OUT/dmesg-before.tmp" && mv "$OUT/dmesg-before.tmp" "$OUT/dmesg-before.txt"
grep -i -E 'oops|bug:|call trace|panic|unable to handle|bad mode|unhandled|segfault|tx-isp|isp[_ -]|vpu|helix|soc_vpu|fault|watchdog|error' \
    "$OUT/dmesg-after.txt" >"$OUT/dmesg-grep.txt" 2>/dev/null
if grep -i -q -E 'oops|call trace|panic|unable to handle|bad mode' "$OUT/dmesg-after.txt"; then
    note "KERNEL OOPS/PANIC TEXT FOUND in dmesg (see dmesg-grep.txt)"
else
    note "no oops/panic text in dmesg"
fi
cat /proc/jz/isp/isp-m0 >"$OUT/isp-m0-after.txt" 2>/dev/null
[ -n "$ISPM0" ] && true
{
    echo "results:$SUMMARY"
    echo "hung: $HUNG"
} >"$OUT/stage-results.txt"
for f in "$OUT"/*.log "$OUT"/*.txt; do
    [ -f "$f" ] && redact <"$f" >"$f.r" && mv "$f.r" "$f"
done

# restart the streamer before packing, so the camera is productive again
restart_streamer

( cd "$WORK" && tar cf - result | gzip -9 >"$TAR" ) 2>/dev/null
if [ -s "$TAR" ]; then
    SUM=$(sha256sum "$TAR" 2>/dev/null | cut -d' ' -f1)
    say ""
    say "[t30kit] done. Result archive: $TAR"
    say "[t30kit] sha256: $SUM"
    say "[t30kit] Review it before sending:  tar tzf $TAR   (no pictures, no config files, no credentials are included)"
else
    say "[t30kit] could not create the tar.gz; the files are in $OUT"
fi
if [ "$HUNG" = 1 ]; then
    say "[t30kit] WARNING: a stage hung or timed out. If the camera is unresponsive, power-cycle it."
fi
exit 0
