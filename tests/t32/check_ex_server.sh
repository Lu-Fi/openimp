#!/bin/sh
# check_ex_server.sh [src/t32]   (Lu-Fi/openimp#782)
# Source guard: the multi-process server serves the stand-alone encoder like vendor 2.0.2.1
# __jz_enc_msg_process / __jz_inputJpege_msg_process (QEMU proof: audit/issue782): YuvInit / YuvEncode /
# YuvExit and InputJpege are called, the client's physical addresses go through IMP_Encoder_VbmP2V, and a
# dead client's encoder is released.  Before #782 every request was answered "failed".
d=${1:-$(dirname "$0")/../../src/t32}
f=$d/openimp_t32_enc_e3.c
[ -f "$f" ] || { echo "usage: $0 [src/t32]" >&2; exit 2; }
fail=0
sv=$(awk '/^static uint32_t mp_yuv_serve\(struct yuv_msg \*m\)$/{p=1} p{print} p&&/^}/{exit}' "$f")
for c in 'IMP_Encoder_YuvInit(&h' 'IMP_Encoder_YuvEncode(mpy.h' 'IMP_Encoder_YuvExit(mpy.h' \
         'IMP_Encoder_VbmP2V((intptr_t)f.phyAddr)' 'IMP_Encoder_VbmP2V((intptr_t)yuv_get(m, YUV_OUTPA))' 'mpy.n >= 1'; do
  echo "$sv" | grep -qF "$c" || { echo "check_ex_server: mp_yuv_serve lacks: $c"; fail=1; }
done
jt=$(awk '/^static void \*mp_jpeg_thread\(void \*arg\)$/{p=1} p{print} p&&/^}/{exit}' "$f")
echo "$jt" | grep -qF 'IMP_Encoder_InputJpege(src, dst' || { echo "check_ex_server: mp_jpeg_thread does not call InputJpege"; fail=1; }
grep -q 'IMP_Encoder_YuvExit(mpy.h);' "$f" && grep -q '^static void \*mp_yuv_watch' "$f" || { echo "check_ex_server: no dead-client release"; fail=1; }
[ $fail = 0 ] && echo "check_ex_server: OK"
exit $fail
