# apitest - every non-image vendor IMP/SU function, per SoC (OpenIMP libimp)

Device test for T10, T20, T21, T23, T31 and T41, sibling of `tools/imgfx` (which
covers the image-changing ISP functions). It calls the vendor API of the SoC
(structs from `ingenic-headers`) against the camera's own libimp and prints one
line per function:

    [A] <function> ret=<r> <PASS|FAIL|N/A|SKIP> <reason>

* `PASS`/`FAIL`: the call and a check of its effect (read back, stream parsed, ...).
* `N/A`: the function is declared in the vendor header but not exported by that
  libimp (every vendor function is weak, one source builds everywhere); also
  `ENOTSUP` from `IMP_Encoder_GetChnEvalInfo`.
* `SKIP`: deliberately not run (reason on the line): writes hardware/RTC/LED,
  reboots, blocks, or needs a topology of its own.
* `FAIL OVERWRITE`: the function wrote outside the vendor struct of this SoC.
  Every out buffer sits in an arena with 64 guard bytes on both sides.
* `[T]` lines: area headers and a summary (`PASS/FAIL/N/A/SKIP` per area,
  distinct functions exercised). A watchdog ends the run with `[E] watchdog:`
  (naming the last function) after 60 s without a result line.

Stop the streamer first (`/etc/init.d/S95timps stop`); the test owns the ISP,
FrameSource and encoder. RAM use is small (FS ch1 640x360 x3 buffers, encoder
640x360 @ 15 fps, 512 KB test buffer, 64 KB mem pool); nothing is written to
`/tmp`. The scene may be anything, but static and lit gives meaningful luma and
motion results.

    apitest SENSOR I2C_ADDR W H [areas]    areas = comma list of sys,fs,enc,osd,ivs,audio,isp,su
    apitest --list                         vendor functions of this build's table (host-runnable builds only)

FS ch0 = sensor size W x H (as in imgfx), FS ch1 = 640x360 scaled; all encoders
and IVS/OSD use ch1. Env: `APITEST_MODE=night` (ISP running mode night),
`APITEST_AIDEV=n` (audio input device, default 1, T23 0), `APITEST_AO=0` skips the
AO return-code checks.

## What is exercised

| area | functions |
|---|---|
| sys | Init/Exit, GetVersion, GetCPUInfo, GetTimeStamp, RebaseTimeStamp (+restore), ReadReg32 (CPM base), MemPoolRequest/Free (T23/T31/T41), Bind/UnBind/GetBindbyDest (FS->ENC and FS->OSD->ENC, FS->IVS) |
| fs | Create/Destroy/Enable/Disable (+cycle), Get/SetChnAttr round trip, FrameDepth, FifoAttr, Delay/MaxDelay, Pool, GetFrame/ReleaseFrame (size, luma, timestamps, rate), SnapFrame, GetTimedFrame, ChnStatQuery/SetChnRotate (T31), Direct mode (T23), I2dAttr/GetFrameEx (T41) |
| enc | H.264 (all), H.265 (T21/T23/T31/T41), JPEG: Group/Chn create/register/start/stop, Polling/GetStream/ReleaseStream, Query; streams parsed: H.264 SPS (resolution/profile/level), PPS, IDR, H.265 VPS/SPS/PPS/IDR, JPEG SOI/SOF/EOI; RequestIDR, Get/SetChnFrmRate (+measured rate), RC attr / BitRate, GOP (SetGOPSize or GopAttr/GopLength), QP bounds, ROI (old: SetChnROI, T41: RoiAttr), MaxPictureSize, FrmUsedMode, SuperFrame, Denoise, HSkip, Demask, Color2Grey, MbRC, ChangeRef, Fisheye, QpgMode, H264/H265TransCfg, InsertUserData (SEI seen), GDR/framelossthd/Crop/InitQP (T23), MaxStreamCnt, FlushStream, GetFd, GetChnEncType, Pool, StreamBufSize, PollingModuleStream, ChnAveBitrate, EvalInfo, bufshare (JPEG on the H.264 buffer), JpegeQl live (JPEG size must follow the table), AvpuJpegQp (T41), software JPEG/YUV encoder (Vbm*, InputJpege, Yuv*; T23/T41) |
| osd | SetPoolSize, Create/Destroy group, regions RECT / PIC (BGRA) / COVER: Create/Register/SetRgnAttr/GetRgnAttr/SetGrpRgnAttr/GetGrpRgnAttr/Show/Start/Stop/UnRegister/Destroy, UpdateRgnAttrData, SetRgnAttrWithTimestamp, T23/T41: GetRegionLuma, Rgn*_Query, SetMosaic, T41 SetGroupCallback; encoder keeps running (check) |
| ivs | Group/Chn create/register/start/stop, move interface (Create/Destroy, PollingResult, GetResult, ReleaseResult, Get/SetParam), base-move interface |
| audio | AI only on the microphone: Pub attr, Enable/EnableChn, ChnParam, PollingFrame/GetFrame/ReleaseFrame (rms, timestamps), Vol/VolMute/Gain/AlcGain/DigitalGain, Ns, Hpf (+cut-off), Agc (+mode), Algo/Hs, GetFrameAndRaw (T41); AENC/ADEC G.711A software round trip of a real mic frame. AO: return codes only (muted, `IMP_AO_SendFrame` is never called, nothing is played). AEC functions are `SKIP` (speaker path) |
| isp | non-image getters and round trips: sensor attr/register, FPS, running mode, anti-flicker, max again/dgain, Expr, total gain, EV, integration time, WB / WB statistics, AWB cluster/ct/trend, AE luma/min/state/attr/targets/hist/zone/weight, AF hist/zone/metrics/weight, AWB hist/zone, blc, frame drop, default bin path; T41: statistics/exposure-info/convergence getters |
| su | libsysutils (dlopen): GetModelNumber, GetVersion, GetDevID, Get/SetTime (same value), SUTime2Raw/Raw2SUTime, Set/Get/DisableAlarm, ADC, battery (where the SDK has it), Key open/close. Never called: Shutdown, Reboot, Suspend, EnableAlarm, PollingAlarm, LED, cipher |

## On-camera commands (binary = this directory, libimp = the camera's own)

Same cameras and sensor arguments as imgfx:

| camera | binary | command |
|---|---|---|
| T10 secuplug 192.168.10.31 | apitest_t10 | `apitest_t10 jxh42 0x30 1280 720` |
| T20 wyze campan1 192.168.10.163 | apitest_t20 | `apitest_t20 jxf22 0x40 1920 1080` |
| T21 victure pc420 192.168.10.27 | apitest_t21 | `apitest_t21 jxf23 0x40 1920 1080` |
| T23 jooan a6m 192.168.10.30 | apitest_t23 | `apitest_t23 sc1a4t 0x30 1280 720` |
| T23 galayou y4 192.168.10.28 | apitest_t23 | `apitest_t23 sc2336 0x30 1920 1080` |
| T31 wuuk y0510 192.168.10.21 | apitest_t31 | `apitest_t31 sc4336p 0x30 2560 1440` |
| T41 vanhua t55a 192.168.178.179 | apitest_t41 | `apitest_t41 gc5603 0x31 2880 1620` |

Typical run:

    scp apitest_t31 root@192.168.10.21:/tmp/
    ssh root@192.168.10.21
      /etc/init.d/S95timps stop
      /tmp/apitest_t31 sc4336p 0x30 2560 1440 | tee /tmp/apitest.txt     # about 2 minutes
      # single areas: apitest_t31 sc4336p 0x30 2560 1440 enc,osd
    scp root@192.168.10.21:/tmp/apitest.txt cam-t31-apitest.txt
    grep -E ' (FAIL|N/A) ' cam-t31-apitest.txt          # what needs attention

Reboot or restart the streamer afterwards (`/etc/init.d/S95timps start`). Exit
status is 1 when any line is FAIL.

## Build

`tools/apitest/build.sh [T10|T20|T21|T23|T31|T40|T41]` cross-builds `apitest_<soc>` and `tools/imgfx/imgfx_<soc>` (and
`t40_devtest` for T40) into `tools/apitest/build/`. Vendor headers come from `timps/include`; the toolchain and a libimp
to link against from `thingino-out/<cam>-agg46-*` (override with `HDR`, `TO`, `GEN`). The binaries use the libimp of the
camera they run on. T40 is built as the T41 API family (`-DPLATFORM_T41 -DAPITEST_T40`, T40 1.3.1 headers); imgfx has no
T40 build (the T40 image functions are covered by `tests/t40/devtest/t40_devtest.c`). `gen_have.sh` regenerates
`apitest_have.h`/`apitest_weak.h` (functions declared per SoC); `gen_rt.py` regenerates `at_isp_rt.c` (same-value Get/Set
round trips of the ISP pairs that `at_isp.c` does not round-trip itself).

## Evidence runs (`run_cam.sh`, `results2json.py`)

    tools/apitest/run_cam.sh 192.168.10.34 T41 [outdir]

Per camera, under the cam lock for the whole run: preflight (sensor, libimp md5, init script, dmesg baseline), stops the
streamer, copies the binaries to `/tmp/at` (< 1 MB), runs `apitest` (areas sys,fs,enc,osd,ivs,audio,isp,su; audio is
microphone only, `APITEST_AO=0`: no AO call at all), `imgfx` with `IMGFX_STATS=1` (the frame is measured in memory: mean
Y/U/V, detail energy, 8x8 luma grid; no picture is written or leaves the camera; `RISKY=1` adds the bypass/FPS cases),
on T40 `t40_devtest` (phases `T40_MASK`, default `cdff`), then removes `/tmp/at`, restarts the streamer, checks dmesg and
writes `result.json` (per evidence function key: PASS / FAIL / PART / SKIP / NA with the detail line). It refuses to run
while `audit/ab-t41/USER_IN_CELLAR` exists. Not done, by design: speaker/AO/AEC, flash, reboot, module load/unload, light.
