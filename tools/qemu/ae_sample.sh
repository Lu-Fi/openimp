#!/bin/sh
# ae_sample.sh <span_frames> [timeout_s] [interval_s] [luma 1|0]  (runs INSIDE the QEMU guest, busybox sh)
# As fast as the guest allows (interval default 0; the emulated guest needs about 1.5-2.5 s per row), until the ISP
# frame counter has advanced span_frames from the first row or timeout_s of guest uptime have passed, one line per row to stdout and
# /tmp/ae_sample.csv:
#   AE,uptime,frames,it_lines,again,dgain,ispdgain,rgain,bgain,ct,ev,luma,mode
# frames = "ch0 done" of /proc/jz/isp/isp-m0 (ISP frame counter; the T20 driver has none: the ISP interrupt count of
# /proc/interrupts, one per frame), mode = ISP running mode (Day/Night), it/again/dgain = sensor registers as the
# driver reports them, rgain/bgain = ISP WB weighted gains (256 = 1.0x on T31), luma = the camera daemon's ae_luma
# (GET /control daynight block, the ISP AE statistics mean), empty while the daemon does not answer.
MF=${1:-1500}; TMO=${2:-300}; IV=${3:-0}; LU=${4:-1}; OUT=/tmp/ae_sample.csv; : >$OUT
TOK=
FR=0; FR0=; U0=$(cut -d. -f1 /proc/uptime)
while [ $(( FR - ${FR0:-FR} )) -lt "$MF" ]; do
  UP=$(cut -d' ' -f1 /proc/uptime)
  [ $(( ${UP%.*} - U0 )) -gt "$TMO" ] && break
  [ -z "$TOK" ] && TOK=$(head -n1 /run/timps.token 2>/dev/null | tr -cd 0-9A-Za-z)
  M=$(cat /proc/jz/isp/isp-m0 2>/dev/null)
  L=; [ "$LU" = 1 ] && L=$(curl -s -m 3 -H "X-Timps-Token: $TOK" http://127.0.0.1:8880/control 2>/dev/null | sed -n 's/.*"ae_luma":\([0-9.]*\).*/\1/p')
  ROW=$(echo "$M" | awk -v up="$UP" -v l="$L" '
    function v(x) { sub(/^[^:]*: */, "", x); sub(/ .*/, "", x); return x }
    /^SENSOR Integration Time/ {it=v($0)}
    /^SENSOR analog gain/ {ag=v($0)}
    /^SENSOR digital gain/ {dg=v($0)}
    /^ISP digital gain/ {ig=v($0)}
    /^ISP WB weighted rgain/ {rg=v($0)}
    /^ISP WB weighted bgain/ {bg=v($0)}
    /^ISP WB color temperature/ {ct=v($0)}
    /^ISP EV value:/ {ev=v($0)}
    /^ISP Runing Mode/ {mo=v($0)}
    /ch0 done/ {split($0,a,"ch0 done "); split(a[2],b,","); fr=b[1]}
    END {printf "AE,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", up,fr,it,ag,dg,ig,rg,bg,ct,ev,l,mo}')
  if [ -z "$(echo "$ROW" | cut -d, -f3)" ]; then   # no "ch0 done" in isp-m0 (T20): ISP interrupts = frames
    IR=$(awk '$NF=="isp" {print $2}' /proc/interrupts)
    [ -s /tmp/ae_base ] && IR=$(( ${IR:-0} - $(cat /tmp/ae_base) ))   # interrupts since the stream was restarted
    ROW=$(echo "$ROW" | awk -F, -v OFS=, -v f="${IR:-}" '{$3=f; print}')
  fi
  echo "$ROW" | tee -a $OUT
  FR=$(echo "$ROW" | cut -d, -f3); FR=${FR:-0}
  [ -z "$FR0" ] && [ "$FR" -gt 0 ] && FR0=$FR
  [ "$IV" != 0 ] && sleep "$IV"
done
echo AE_DONE
