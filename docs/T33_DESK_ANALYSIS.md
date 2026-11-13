# T33 desk analysis (no hardware, no OpenIMP build yet)

Status: documentation only, 2026-11-03 (issue #98). Nothing here has run on a camera or in QEMU. The facts come
from the vendor documentation set, the vendor SDK 2.0.0 (T33 / T32Pro line) binaries (`nm`, `readelf`, `strings`;
no vendor code or tables are copied) and the existing T32 work in `open-tx-isp` (`driver/t32`). The T32 work is the
template: T33 is a close variant of T32, so most of the plan is "make `driver/t32` serve both".

## 1. SoC facts

| Item | T33 | Compared with |
|---|---|---|
| CPU | XBurst1, 1.0 GHz, 32 KB L1 I/D, 128 KB L2, SIMD128 (MXU2), 9-stage | T32: 1.2 GHz, same core family; same as T31 |
| MCU | RISC-V coprocessor, 500 MHz (AOV / fast start) | T32: 600 MHz |
| ISP | Tiziano 4.1 family, 5 MP at 25 fps, up to 2880x1620 / 2592x1944, 3 outputs, LDC + HLDC + I2D (mutually exclusive), no WDR, no DVP | T32: Tiziano 4.0, WDR and DVP supported; T32Pro: 4.2 |
| Encoder | "Hera v1.6": H.264 / H.265 / JPEG, up to 6 MP at 25 fps; kernel side `soc_vpu` with the two blocks the vendor kernel calls Radix and Helix (not the Allegro AVPU) | T32 / T30 / T23 family encoder, not T31 / T40 / T41 |
| Memory | DDR2 / DDR2L / DDR3 / DDR3L in package: 512 Mbit (L, ZL, DL) or 1 Gbit (N, ZN) per the vendor version note | thingino maps N, ZN, VN to 128 MB and the rest to 64 MB |
| NPU | 0.5 TOPS int8 (not used by this project) | |
| Variants | L, N (regular), ZL, ZN (low-power battery), VL, VN (AOV / "Atlas"), DL (video lock), A (external DDR) | thingino lists t33dl t33l t33lq t33n t33vl t33vn t33zl t33zn |
| Not on T33 | DMIC, DVP, USB OTG, eMMC 8 bit | T32Pro has DMIC and eMMC 8 bit |
| Package | QFN96, pin compatible with T32 / T32Pro | VDD 0.9 V typical (T32Pro 0.8 V) |

Naming in the vendor trees: T32 = `PRJ007`, T33 = `PRJ008`, T32Pro = `PRJ009`. One Linux base (3.10.14, 4.4.94 and
5.15.170 are all shipped) and one libimp serve T33 and T32Pro (SDK line "2.0.0"); the older T32 / T33 line ends at
2.2.0 (T32) / 2.0.2.1 (T33).

Kernel side (3.10.14 tree): `soc-PRJ008` has the same register base and interrupt layout as `soc-PRJ007` (ISP core,
VIC, IVDC, LDC, I2D, Radix / Helix, MIPI CSI at identical addresses; a diff of `base.h` and `irq.h` shows no
difference except a removed display base). Differences are in clock tree, GPIO, platform data, power management and
defconfigs (T33 has DMA modes for I2C / AES / hash / AIC enabled; `CONFIG_SOC_VPU` with Helix and Radix is on in
both). The T33 ISP driver glue is the same source file as T32 with `CONFIG_SOC_PRJ008` branches for four values
(see section 4); the ISP core itself is a prebuilt firmware object per kernel version.

### ISP generation: what differs from T32 inside the ISP firmware

Symbol comparison of the two prebuilt ISP firmware objects (function symbols only):

- 1023 functions are common, 426 exist only on T32, 598 only on T33.
- The T33 object routes (nearly) all block programming through register lists: about 48 `rcfg` functions (T32: 9),
  one `tisp_rcfg_<block>_reg_init` per block (AE, AF, AWB, BCSH, BLC, CCM, CDNS, CLM, CSC, CSCCR, defog, DG, DMSC,
  DPC, gamma, IPC, LCE, LDC, LSC, LSF, MDNS, MSCA, RDNS, SDNS, TMO, YDNS, YSP) plus T33-specific reset / switch-interrupt
  / write-all functions. On T32 the list is used for one block (LSC at stream-on).
- T33 adds a statically linked AE library (about 80 `ae_*` functions, including a multi-camera "joint" part), BLC
  functions (14 new), CLM, and more RISC-V / IPC helpers (AOV).
- T32 has WDR (about 38 `tisp_wdr` functions), a different TMO / LCE implementation (50 vs 15 TMO functions) and GIB / DPC /
  SDNS pieces that T33 folds into other blocks. Block names must be mapped, not assumed absent.
- IQ bin: same container as T32 (28-slot section table at 0x14, project name `PRJ008` at 0x98, sections from 0xa0).
  The AE and AWB sections have the same size as on T32, the other sections are smaller, two slots are empty (T33 has no
  WDR section) and the LSC section has another size. So `tx_isp_t32_iq.h` needs per-SoC section sizes and the payload rules
  must be re-derived for T33; the T32Pro (`PRJ009`) bin is a different, compact format and is out of scope here.
- ioctl ABI: T33 and T32 share one header in the vendor tree (101 ioctl definitions). T32Pro has a reorganised
  header. So the T32 ioctl table in `driver/t32` is expected to be valid for T33; verify against the T33 libimp when
  the first trace exists.

## 2. Vendor software available locally

| Part | What exists | Use |
|---|---|---|
| libimp | glibc and uClibc builds, shared and static, unstripped, with debug information; 575 `IMP_*` exports (+62 `SU_*` in libsysutils) | export list, structure layouts, flow RE |
| ISP driver | glue source (shared with T32), prebuilt ISP firmware objects for kernels 3.10 / 4.4 / 5.15, with and without PM, prebuilt `.ko` | ioctl ABI, clock / memory parameters, function-level RE |
| Kernel | 3.10.14, 4.4.94, 5.15.170 with `soc-PRJ008`, defconfigs (goat, vela, atlas, nand variants) | platform data, clocks |
| Boot | U-Boot 2013.07 and 2022.10, option names `PRJ008_<model>_sfcnor` | firmware profile |
| IQ bins | 6 sensors for PRJ008 (day / night pairs) | parser work, T33 reference |
| Sensor drivers | about 100 sensor driver sources (most shared with T32) | sensor modules |
| Docs | SDK guides (media, ISP, BSP, bit rate control, smart encoding, sensor adaptation), datasheets, HDK | local archive only |

Vendor libraries, firmware objects and IQ bins stay local; this document contains function-level facts only.

## 3. libimp exports and the backlog issues (#46 to #60)

Source: `nm -D --defined-only` of the T33 (SDK 2.0.0) `libimp.so` (glibc and uClibc builds export the identical 575 names;
`tools/export-check/vendor-T33.txt` is the list including the 62 sysutils names, checked: no difference).

Exports by area (IMP prefix): ISP 167, Encoder 120, FrameSource 49, AI 47, AO 37, OSD 34, DMIC 28, IVS 18, System 13, ADEC 9,
Decoder 8, AENC 8, buffer / pool / memory helpers about 25.

Evidence-matrix state for T33 (`docs/api_evidence.json`, 658 rows): 502 rows `nob` (exported by the vendor libimp, no
OpenIMP build yet), 156 rows `n.a.` (not exported). Cross-check done for this document: every `n.a.` row is absent from the
vendor export list, every `nob` row is present; zero mismatches. Rows per area:

| Area | exported (`nob`) | not exported (`n.a.`) | of the `nob` rows: needed by an application (first build) | backlog |
|---|---|---|---|---|
| ISP tuning | 95 | 65 | 36 (#46) | 59 |
| AE / AWB / AF | 31 | 41 | 4 (#47) | 27 |
| Encoder | 103 | 28 | 25 (#48) | 78 |
| FrameSource | 42 | 6 | 15 (#49) | 27 |
| OSD | 39 | 4 | 19 (#50) | 20 |
| IVS | 17 | 0 | 17 (#51) | 0 |
| Audio | 75 | 4 | 50 (#52) | 25 |
| System / sysutils / log | 100 | 8 | 12 (#53) | 88 |

So every function the issues list exists in the T33 vendor libimp; none of them is `n.a.`. No application-used row is
missing from T33.

Compared with the other SoCs (vendor export lists): 19 names exist on T33 but not in the T32 2.2.0 list (Decoder x8,
`Encoder_Get/SetChnRfcRatio`, `Encoder_Get/SetChnRfcDropFrameLines`, `FrameSource_CreateChn_Ext`,
`FrameSource_SetSubStreamOnlineMode`, `ISP_RawCaptureGuardBegin/End`, `ISP_Tuning_GetISPRunningModeWithFd`, two
`IMPDBG_DeInit`); two `SU_CIPHER_*` buffer helpers exist only on T32. Against T41, T33 lacks (not exported): the
emulated FrameSource set, GOP / ROI attribute and JPEG-quality-level encoder calls, AE / AWB convergence-step and AE
scene calls, TMO curve get / set, the raw read / write control, the ISP GPIO calls and the VIC-done callback, and the
old log macros-as-functions. Against T40 it additionally lacks the ISP OSD / draw attribute calls in their T40 form, mask
get / set, `ISP_Bypass_Bind` and the scaler-level call.

Caveat for the matrix: "exported" is not "functional on T33". The vendor SDK difference notes say that the following are
supported by T32Pro only although the shared libimp exports them: the Decoder group (JPEG decoding), the RFC ratio /
drop-lines calls, `FrameSource_SetSubStreamOnlineMode`. WDR is not supported on T33 although the `WDR_*` and
`Get/SetWdrOutputMode` calls are exported. IPU colour-space conversion and the IPU OSD 2-bit mode are not supported on T33.
Expect these to return errors on hardware.

### Proposed matrix changes for T33 (not applied)

1. Cells: none. The `n.a.` and `nob` split already equals the nm evidence for all 658 rows.
2. Notes (text only, attach when a T33 build exists): the Decoder group, the four RFC ratio / drop-lines rows and
   `FrameSource_SetSubStreamOnlineMode` stay `nob` with a note "exported by the shared T33 / T32Pro libimp; vendor notes: T32Pro
   only"; the WDR rows get "exported, T33 has no WDR". Once a T33 build exists and a call fails on purpose, the existing value
   `err` ("exported but fails or has a known defect") is the right cell; no new vocabulary is needed.
3. T32 column: the 19 T33-only names are an SDK-line difference (2.0.0 vs 2.2.0), not hardware. If the T32Pro line is ever
   adopted for T32, those rows change from `n.a.` to `nob` there.
4. Feature matrix (main table) T33 cells stay "planned, not started" until stage T33-S1 below runs in QEMU.

## 4. Kernel, driver layout and what a first build needs

### Kernel and drivers

- Kernel: 3.10.14 is the default for the vendor image; 4.4.94 and 5.15.170 exist. The thingino fork has branches
  `ingenic-t33` plus tagged variants for 3.10.14, 4.4.94 and 5.15.170 (SDK 2.0.2.1 level); the firmware tree does not use
  them yet (section 5).
- ISP module name `tx-isp-PRJ008.ko` (T32: `tx-isp-PRJ007.ko`). Module parameters of the vendor glue, which the open
  driver must accept or ignore: print level, parent clock names for ISP / scaler / AXI / video / LDC, clock rates, `isp_memopt`
  (default 5), pre-dequeue time, `direct_mode`, IVDC memory line / stride / threshold, `zeratul_open` (fast start),
  `atlas_open` (AOV, follows `CONFIG_PM`), cache line, stream-off mode.
- T33-specific parameter defaults in the vendor glue (T32 values in brackets): ISP clock 340 MHz [219], AXI / video clock `isp_clkv` 340 MHz
  [500], LDC clock 357 MHz [588], IVDC stride 3200 [3840], two parent clocks (`sclka`, `mpll`) [three, plus `vpll`],
  sensor external clock `ext` on 4.4 / 5.15 and `ext1` on 3.10 [`vpll`].
- Device nodes libimp opens (same set as T32): `/dev/tx-isp`, `/dev/isp-m0`, `/dev/isp-tuning`, `/dev/tisp`,
  `/dev/framechan%d`, `/dev/misc-ivdc`, `/dev/offline_ldc`, `/dev/i2d`, `/dev/ipu`, `/dev/soc_vpu`, `/dev/kern_enc`,
  `/dev/rmem`, `/dev/mpsys_data`, `/dev/mem`; NPU `/dev/ingenic-npu`.
- Encoder: libimp contains its own software-side encoder front end (`i264e` / `i265e` flows) driving `/dev/soc_vpu`; no AVPU.
  `kern_enc` is only the AOV fast-start path. The encoder HW (QEMU model notes: command lists, T33 lists give the
  reconstruction address of an intra picture, other than T32) is the largest reverse-engineering item and is shared with
  T32.

### First OpenIMP build for T33

| Need | Status |
|---|---|
| Toolchain | vendor gcc 5.4.0 (glibc 2.22 or uClibc 0.9.33.2), MXU2; thingino's xburst1 toolchain builds the same as for T31 / T32 (`-mips32r2 -mabicalls`, uClibc or glibc). No new toolchain. |
| Build script | `build-t33.sh`, derived from the T32 script that does not exist yet either: OpenIMP has no T32 build. The order is T32 first (E0 to E2 of the T32 plan), T33 as a variant. |
| Headers | T33 libimp headers (shared with T32 except the 19 names above); struct layouts from the libimp debug information |
| ISP / FrameSource | T41-family tuning layer + `framesource_tseries` path as in the T32 plan; T33 flags: no WDR, no DVP, LDC / HLDC / I2D exclusive |
| Encoder | helix / radix based path shared with T32 (JPEG, H.264, H.265), HERA command-list builder |
| Audio | T41-style API; no DMIC on T33 |
| Evidence | `vendor-T33.txt` exists; `make export-check` needs `T33` in `SOCS` once a build exists |
| Driver | `open-tx-isp` `driver/t32` with a T33 switch (section 6, stage T33-S1) |

## 5. Firmware profile (thingino), read-only inventory

Checked in the local firmware checkout (branch `ciao`), nothing changed:

| Item | State |
|---|---|
| SoC description | `soc/ingenic/t33.mk` exists: eight models, `SOC_UBOOT := isvp_t33_sfcnor` for all, RAM 64 MB (L, ZL, VL, DL, LQ) / 128 MB (N, ZN, VN). Per-model U-Boot names like the vendor `PRJ008_<model>_sfcnor` are not used. |
| Model list | `Config.soc.in` knows all eight T33 models |
| Kernel | `thingino.mk` has no T33 case: falls to `ingenic-t31`. Upstream thingino-linux has `ingenic-t33` and tagged T33 branches; the vendor tree has a separate `soc-PRJ008`. `docs/media/ingenic-sdk-versions.md` documents "t33 = ingenic-t31 (shared)". |
| Libimp blobs | `ingenic-lib.mk` has no `t33` version case (`SDK_VERSION` stays empty); the upstream `ingenic-lib` has `T33/lib/2.0.2.1` (glibc and uClibc), not the newer 2.0.0 line |
| SDK drivers / IQ | the pinned `ingenic-sdk` has no T32 or T33 ISP / sensor-IQ tree; T32 needs the workarounds listed in the T32 plan |
| Camera profiles | none for T33. T32 has two (one testing, one experimental) and a QEMU one, a good template |
| Chip detection | `package/thingino-system/files/ingenic/soc` recognises T33 variants by the id byte (L, N, A, ZL, ZN, LQ, VL, VN) |
| U-Boot | `configs/uboot/variants/` has `t32nq.config` only |
| ISP clock menu | `Config.soc.in` has T32-specific clock defaults and no T33 ones |
| Audio / USB DT injection | scripts already know "T33 has no on-chip codec node" |

Files a T33 profile needs (our fork, cameras-testing first; build-system parts stay local per the firmware-scope rule and are
reported to the maintainers, only open-stack parts are PR material):

1. `thingino.mk`: kernel branch for `t33` (3.10.14 first, tag/branch `ingenic-t33`), `KERNEL_SOC` handling, DTS destination name.
2. `package/ingenic-lib/ingenic-lib.mk`: `t33` case with `SDK_VERSION` 2.0.2.1 (or the 2.0.0 line once uploaded upstream); libimp must match
   the ISP firmware version in use.
3. `soc/ingenic/t33.mk`: per-model U-Boot names; `configs/uboot/variants/t33*.config`; U-Boot 2022.10 or the thingino U-Boot with `t33` support.
4. `Config.soc.in`: ISP clock defaults from the vendor glue (340 MHz class, two parent clocks).
5. SDK overlay: T33 `tx-isp` glue, T33 sensor modules, six `*-PRJ008-*.bin` IQ files, `sdk/t33` firmware objects.
6. `configs/cameras-testing/<board>_t33l_<sensor>_*/`: defconfig, dtsi (reuse the T32 wiring pattern), `thingino.json`, `uenv.txt`;
   a QEMU profile `qemu_t33` for desk work.
7. Open-stack packages: `open-tx-isp` T33 target (`tx_isp_t33` module name, same loader line as T32) and OpenIMP once built.

## 6. Reuse from the T32 work

| T32 piece (`driver/t32`, `tests/`) | Reuse for T33 |
|---|---|
| Device nodes, ioctl dispatch, sensor registration, `/proc/jz` text | as is; ABI shared (verify) |
| `sinfo` (sensor registry), VIC / CSI derivation from sensor attributes | as is |
| Frame channels, done FIFO, ISP / VIC interrupt handling | as is; T33 interrupt map is the same |
| MSCA geometry and polyphase curves | likely as is (same 70-function MSCA family); confirm against the first T33 trace |
| RCFG list builder / walker (`tx_isp_t32_rcfg.h`) | becomes central: T33 builds one list per block; needs an all-blocks list mechanism and the T33 trigger / switch-interrupt flow |
| IQ framework (28-slot table) | reuse, but T33 section sizes and look-up payload rules are new |
| AE / AWB statistics engines, AE / AWB sections | candidates for direct reuse (same section sizes); engine register offsets to confirm |
| LSC, gamma, DMSC, CDNS, YDNS, SDNS, YSP, CLM, GIB, MDNS, DPC, front end (CSC / CSCCR / top) | block logic reusable only after the T33 section layout is decoded; each block must be re-checked write for write against a T33 vendor trace |
| TMO, LCE, RDNS, BCSH, defog, AF | not built on T32 either |
| WDR | not on T33 (skip); T32 has no WDR work either |
| Host tests with bin + trace comparison | same method; needs a T33 vendor trace (QEMU `ingenic-t33` machine exists) |
| `tools/t32_msca_sinc_tab.py` | as is |

## 7. Ranked plan

Stages follow the T32 scheme. Prerequisite for all: T32 stages S4 / S5 (stream-on lists, tisp blocks, 3A) and the T32 OpenIMP
steps (E0 to E2) are the base; T33 should not start in parallel with them.

| Stage | Content | Needs a camera? |
|---|---|---|
| T33-S0 | This analysis (done). Next desk step: T33 vendor flow under QEMU `ingenic-t33` (libimp 2.0.0, ioctl / register trace), one IQ bin, one sensor module | no |
| T33-S1 | `driver/t32` gets a T33 switch: clock / stride defaults, two-parent clocks, per-SoC IQ section table, module name; vendor libimp on our `.ko` in QEMU delivers frames (like T32 S2 / S3) | no |
| T33-S2 | RCFG backend: all-blocks list builder, T33 trigger and switch interrupt, block init from the T33 bin write for write against the vendor trace; AE / AWB statistics | no for tests against the vendor trace; picture effect yes |
| T33-S3 | OpenIMP T33: variant of the T32 build (`build-t33.sh`, platform define, `SOCS` entries, export-check, matrix rows from `nob` to host / QEMU states); WDR / DVP / DMIC / IPU CSC off | no |
| T33-S4 | Firmware profile in the fork (section 5), QEMU image boots with open ISP; HERA / JPEG encoder bring-up on the T32 base | no (QEMU), final proof yes |
| T33-S5 | Real hardware: UART, flash dump, stock A/B reference, sensor bring-up, 3A quality in daylight, bit rate and bitstream checks, LDC / HLDC / I2D, rmmod / insmod robustness | yes |
| T33-S6 | Extras: AOV (RISC-V firmware, `kern_enc`), low power variants (Z / V models), multi-camera. Not part of a first release | yes |

Needs a real T33 camera for: image quality (AE / AWB / tone mapping), encoder correctness of H.264 / H.265 / JPEG at full size,
the stream-on timing and RCFG interrupt flow under real sensor timing, LDC / HLDC / I2D output, power and clock stability, anything
AOV. The QEMU model does not execute T32 / T33 register lists, so even the list-driven parts can only be checked as "list built and run".

Test hardware: one T33L module is on order. Before any device step the T32 checklist applies (UART, flash dump, recovery path, SD card).

## 8. Risks and open questions

- High: the encoder family (Hera / Radix / Helix) is shared with T32 and unsolved; T33 differs in the reconstruction addressing of the lists.
- High: RCFG-everywhere makes T33 a larger RE job than T32 for the ISP (about 30 block init functions through lists).
- Medium: the 2.0.0 and 2.0.2.1 SDK lines differ (libimp 2.0.0 vs the 2.0.2.1 blob the firmware ecosystem has); the open driver must name the SDK line it targets.
- Medium: IQ bin sections for T33 are not decoded yet; wrong sizes mean silent mis-parse.
- Low: CPU / kernel / toolchain (same XBurst1 experience as T31 / T32).

## 9. Encoder on HERA v1.6 (#391, QEMU only)

OpenIMP `build-t33.sh` runs the T32 encoder (`src/t32/openimp_t32_encoder.c`) with these T33 deltas, each taken from vendor
libimp 2.0.2.1 traces in QEMU `-M ingenic-t33` (t33l, cv2005, vendor 3.10.14 kernel):

- H.264 command lists: `src/t32/openimp_t33_hera.c` with the vendor's register order and defaults
  (`openimp_t33_hera_tab.h`, `tools/t32/hera_h264_table.py --t33`). VMAU moved from 0x80000 to 0x40400 (context states
  0x40448), its tail to 0x70000, a new block at 0x38000; the MCE step tables 0x30900 / 0x30a00 are gone (I 776 pairs,
  P 811; T32 736 / 913).
- Reconstruction: compressed, one picture-sized area per plane (luma `w * (h + 64) + 16 KiB`, chroma half) plus two
  sets of compression headers (16 bytes per 64 x 16 block, counts rounded up to 4) written and read in turn. Only the
  I picture names the area (0x10008 / 0x1000c), only the P picture after it names the reference (0x28014 / 0x28018,
  0x30070 / 0x30074); later lists leave them 0.
- The core adds the emulation prevention bytes (SDE 0x68018 bit 7): the slice data is copied, not escaped.
- JPEG: the engine writes into a 2 x 250 KiB section buffer (`limit_bs_len` 0, `bsfull_jpeg_intr_size` 250, START +
  WAIT_BSFULL_COMPLETE), as the vendor; the job is otherwise the T32 one (44 words, identical but the stream address).
- H.265 command lists: `src/t32/openimp_t33_hera_h265.c` with the vendor's order, defaults and QP bands
  (`openimp_t33_hera_h265_tab.h`, `tools/t32/hera_h265_table.py --t33` from a QP 0..51 sweep; I 591 pairs, P 626;
  T32 692 / 840). BC CABAC run of 33 states (T32 145; mapping to the Radix HEVC set found over the sweep), the H.264
  path's compressed area with the header sets laid out Y A, Y B, C A, C B, new ODMA words 0x10170..0x10188 (0x10184 =
  75/128 of the luma picture, 0x10188 = the NV12 picture's bytes on I, the luma base on P), core emulation
  prevention (SDE 0x68000 bit 28, no QP field there), no MCE step tables, MD lambda `0xb0000000 | qp << 20`.
- Stream buffers: process memory (packs with `phyAddr` 0, as the vendor's); rmem holds the engine buffers only, so a
  1080p H.264 channel fits the T33L's 12.4 MiB rmem next to IVDC (vendor: ivpuBs 500 KiB, video0 3.5 MiB, ivdc 3 MiB).
- Section buffers: one 2 x 250 KiB buffer per engine shared by all its channels (HERA "ivpuBs", JPEG "jpegBs"), as
  the vendor; a channel holds the engine's lock from START until its last part is copied.
- soc_vpu node `vtype`: H.264 0x91, H.265 0x80 (vendor 2.2.0 and 2.0.2.1; the radix driver writes it to CFGC 0x0c);
  also set on the T32 now.

Evidence: `make -C tests/t33 check` builds 24 vendor H.264 lists and 66 H.265 lists (QP 0..51 sampled, 1920x1080,
1280x720, 640x360, 1600x900, 1920x720, 1024x576; I, P after I, P after P) word for word (180 of 180 sweep lists
checked during the port); for H.264 the 24 lists cover (1920x1088 QP 30 / 40, 1280x720, 640x368, 960x544, 1024x576,
1600x912, 1920x720; I and P). QEMU: OpenIMP H.264 FIXQP 30 and the default CBR run, 8 / 10 pictures,
decode clean, Y SSIM 0.82-0.89 against the captured sources (vendor 0.83-0.87 on the same scene); JPEG 3 / 3, ioctl
sequence = vendor's but the rmem cache calls.

Open (#391): any device proof (no T33 camera runs the open stack yet); rate control on T33 follows the T32 work (#295).
