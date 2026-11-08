# Open stack (OpenIMP + open-tx-isp) - reference for AI assistants

**Audience:** an AI assistant answering thingino users. State of this file:
2026-11-08, release candidate **RC3 `v2026.11.07-rc3`** (same tag in OpenIMP
and open-tx-isp). Statements marked "since RC3" or with a later date do not
apply to older images. Read [AGENTS.md](../../AGENTS.md) first (honesty rule).

## Where to find what

| Question | File |
|---|---|
| Does feature X work on SoC Y, with which stack? How do I use it? | [feature-list.md](feature-list.md), then the live matrix |
| Something does not work / a log line | [troubleshooting.md](troubleshooting.md) |
| What does `OPENIMP_...` do, what is its default? | [env-vars.md](env-vars.md) |
| What does the module parameter `...` do? | [module-params.md](module-params.md) |
| Everything else: what the stack is, how it is built, released, gated | this file |

## 1. What the open stack is

| Part | What it is | Replaces |
|---|---|---|
| **open-tx-isp** | Linux kernel driver `tx-isp-<soc>.ko` (ISP, frame channels, sensor glue, 3A in the kernel where the vendor has it there) | the vendor `tx-isp-<soc>.ko` |
| **OpenIMP** | userspace `libimp.so` implementing the vendor IMP / SU API (ISP tuning, FrameSource, Encoder H.264/H.265/JPEG, OSD, IVS, audio, DMIC, system) | the vendor `libimp.so` |

- The public API is the vendor IMP API. Applications using the IMP API need no
  code change. Extras beyond the vendor are reachable through the same calls,
  through environment variables ([env-vars.md](env-vars.md)) or through a few
  optional `OpenIMP_*` symbols that an application can probe.
- Sensor modules, the audio module and the AVPU / soc_vpu kernel drivers stay
  the ones of the Ingenic SDK; the open stack replaces only the ISP module and
  libimp.
- Kernels: 3.10.14 for T10, T20, T21, T23, T30, T31; 4.4.94 for T40, T41.
- T10 has no own OpenIMP build: the T20 build detects T10 at run time.
- T32 / T33: **planned**. T32 runs only in QEMU (an open driver skeleton exists,
  no OpenIMP build); T33 has no driver yet. They are not part of RC3 or
  Release 1.
- C100 is not covered.

## 2. How thingino enables it

Packages in thingino-firmware: `package/openimp`, `package/open-tx-isp`,
`package/thingino-isp` (the choice). The choice "ISP stack" in menuconfig
(*System Packages -> ISP stack*):

| Symbol | Meaning |
|---|---|
| `BR2_PACKAGE_THINGINO_ISP_PROPRIETARY` | vendor ISP module + vendor libimp ("Proprietary Ingenic ISP stack") |
| `BR2_PACKAGE_THINGINO_ISP_OPEN` | open-tx-isp + OpenIMP ("Open ISP stack (experimental)" is thingino's menu text) |
| `BR2_PACKAGE_THINGINO_ISP_OPEN_VENDOR_LIBIMP` | with `..._OPEN`: open kernel driver, vendor `libimp.so` (to tell driver problems from library problems) |
| `BR2_PACKAGE_OPEN_TX_ISP_SUPPORTED`, `BR2_PACKAGE_OPENIMP_SUPPORTED` | per-SoC gates (T10..T41 as above, not C100) |
| `BR2_PACKAGE_THINGINO_ISP_GUARD` | optional boot guard `/etc/init.d/S10isp-guard` (branch `aperto` only) |

How the packages build: `open-tx-isp` builds `driver/<soc>` out of tree and
installs `tx-isp-<soc>.ko` (after `ingenic-sdk`, so it replaces the vendor
module). `openimp` runs `build-for-device.sh <SOC>` and installs
`/usr/lib/libimp.so`; a finalize hook re-installs it last.

**Switching between the stacks** means building and flashing a new **full**
image with the other choice (no run-time switch). Example fragment:

```
BR2_PACKAGE_THINGINO_ISP_OPEN=y            # open stack
# BR2_PACKAGE_THINGINO_ISP_PROPRIETARY=y   # vendor stack
```

### Which firmware branch ships which version (checked 2026-11-08)

| thingino-firmware branch (themactep) | Default ISP stack | Pinned open stack |
|---|---|---|
| `master` | vendor (`..._PROPRIETARY`) | an early pre-RC1 state from the original upstream repositories (OpenIMP of 2026-09-08, open-tx-isp of 2026-09-10); OpenIMP gate without T10. Do not use it to judge the current open stack. |
| `ciao` | vendor (`..._PROPRIETARY`); open stack opt-in | **RC3** `v2026.11.07-rc3` of both repositories |
| `aperto` | **open** on SoCs with `BR2_PACKAGE_OPEN_TX_ISP_SUPPORTED`, vendor elsewhere | **RC3** `v2026.11.07-rc3`; also has the boot guard package |

Packagers pin `OPENIMP_VERSION` and `OPEN_TX_ISP_VERSION` in
`package/openimp/openimp.mk` and `package/open-tx-isp/open-tx-isp.mk`. Both
must point to the **same release** (driver and library share ioctl layouts,
control IDs and rmem handling).

## 3. Releases, branches and gates

| Ref | Purpose |
|---|---|
| `next` (Lu-Fi) | integration branch, tracks the tested aggregate, fast-forward only |
| `aperto` | the released open stack, fast-forward only from `next` |
| `main` | the original author's line; not the tested stack |
| tags `vYYYY.MM.DD[-rcN]` | date tags, same tag in both repositories |

Tags: RC1 `v2026.10.11-rc1`, RC2 `v2026.10.25-rc2`, **RC3 `v2026.11.07-rc3`**.

- **RC3 gate (reached 2026-11-07):** every vendor function that a streamer
  calls works on every gate SoC: 2017 of 2017 cells (T10 228, T20 228, T21 252,
  T23 290, T30 230, T31 319, T40 217, T41 253), no open critical/high issue,
  RC3 milestone empty. "Works" includes the evidence level "API round trip"
  (for example `DMIC_*` on T31/T40/T41) and matched vendor no-ops; the matrix
  shows which.
- **Release 1 (not reached):** the image built from the RC3 tag runs 14 days
  without an unplanned reboot or oops on at least one camera per gate SoC,
  plus the fair light-box comparison against the vendor on every SoC (#230).
  A new image or an unplanned reset restarts the counter.
- Rules are machine-readable in `docs/status/release.yaml`.

## 4. Per-SoC summary (snapshot RC3; the live matrix is authoritative)

"All vendor functions" = vendor IMP/SU functions that work (device-tested or a
matched no-op) out of all vendor functions of that SoC.

| SoC | Streamer functions (gate) | All vendor functions | Short state | Main open points / deviations |
|---|---|---|---|---|
| T10 | 228/228 | 304/329 | fully open from a flashed image; H.264, HW JPEG, OSD, IVS, audio + AEC | rotation 🔧 (extension, device test pending); light box: night picture brighter than vendor (accepted) |
| T20 | 228/228 | 304/329 | fully open; OEM rate controller by default | rotation 🔧; light box: temporal noise 15-22 % above the vendor at matched exposure, day AE about 7 % darker (no follow-up yet) |
| T21 | 252/252 | 308/333 | fully open; vendor-identical eprc, reference sharing | rotation 🔧 |
| T23 | 290/290 | 419/457 | fully open with the native Helix encoder | no real WDR; rare single Helix encode error (errno 5); open AWB runs on every 4th ISP interrupt (vendor every one, #233) |
| T30 | 230/230 | 286/350 | gate SoC since 2026-11-03 (one lab camera, SC4236) | AWB presets without effect, RawDRC strength above vendor, Temper deviation (all ⚠️); anti-flicker, OSD, IVS, rate-control modes, audio output, AEC 🔧; 13 cells `?`; picture quality vs vendor not measured |
| T31 | 319/319 | 365/406 | reference SoC; H.264, H.265, HW JPEG | CBR writes no filler (static scenes stay below target); CappedQuality behaves like CappedVBR (less than vendor) |
| T40 | 217/217 | 349/389 | open driver usable end to end; vendor-equal picture in the fair light-box pass (#84) | only the sc830ai board device-tested; audio output and AEC 🔧; at AE comp 200 the analog gain stops at 4095 (vendor 5247) |
| T41 | 253/253 | 392/427 | fully open from a flashed image; H.264 + H.265 | fair light-box comparison still open (#230); no audio hardware on the test board (audio at API level only); AF statistics opt-in (`t41_af=1`) |
| T32, T33 | - | - | **planned** (QEMU only) | not part of RC3 / Release 1 |

## 5. Links

- Website (status per SoC, light-box protocols open vs vendor, trend per camera): https://lu-fi.github.io/openimp/
- Live feature matrix: https://lu-fi.github.io/openimp/feature-matrix.html
- Project board: https://github.com/users/Lu-Fi/projects/1
- Issues: https://github.com/Lu-Fi/openimp/issues (library, encoder, integration; default when unsure) and https://github.com/Lu-Fi/open-tx-isp/issues (kernel driver, ISP, memory)
- Releases: https://github.com/Lu-Fi/openimp/releases and https://github.com/Lu-Fi/open-tx-isp/releases; RC3 note: [docs/releases/RC3.md](../releases/RC3.md)
- Wikis: https://github.com/Lu-Fi/openimp/wiki (Installing-and-Switching, Environment-Variables, Module-Parameters, Memory, Troubleshooting, Supported-SoCs-and-Sensors, Per-SoC-Status, Release-Gates, Reset-Diagnostics, Test-Pattern, Building) and https://github.com/Lu-Fi/open-tx-isp/wiki
- In this repository: [FEATURE_MATRIX](../FEATURE_MATRIX.md), [OPENIMP_BEYOND_VENDOR](../OPENIMP_BEYOND_VENDOR.md), [RESET_DIAGNOSTICS](../RESET_DIAGNOSTICS.md), [TESTPATTERN](../TESTPATTERN.md), [light-box protocols](../lightbox/README.md), [CHANGELOG](../../CHANGELOG.md)

## 6. Accuracy notes

- Counts and cell states above are a snapshot; quote the live matrix for the
  current state and say the date of what you quote.
- Camera names in the project docs are anonymised (cam-A T31, cam-B T23,
  cam-C T20, cam-D T21, cam-E T10, cam-F T41, cam-K T40).
- The thingino `openimp` package help text calls T30 "video-only"; the RC3
  matrix shows T30 audio input ✅ and audio output 🔧. Trust the matrix.
