# Colour-checker analysis (`tools/colorchecker.py`, #102)

Measures colour accuracy of a camera picture of the Datacolor SpyderCheckr 24 (light box, 0/45 degrees) and compares the open
stack (open-tx-isp + OpenIMP) with the vendor stack on the same lamps. Measurement only: it never changes a CCM. Rule: vendor or
better, so open must not be worse than vendor on any patch group (see [lightbox/README.md](lightbox/README.md)).

## Use

Needs Python 3, numpy and Pillow. The SpyderCheckr reference values are licensed by Datacolor and are **not** in this repository.
Provide them as CSV with the header `id,L_orig,a_orig,b_orig` (ids `A1`..`D6`, Lab D50/2 deg, block "Original" of
"SpyderCheckr Default Measurements.txt") with `--ref FILE` or `$SPYDER_REF`.

```
tools/colorchecker.py --ref ref.csv RUN_DIR [--phases p1_day,...] [--out COLOUR.md] [--nv12 W H]
tools/colorchecker.py --ref ref.csv --files open.jpg [more.jpg ...] [--vs vendor.jpg ...]
```

`RUN_DIR` is a light-box run directory with `open/<phase>/snaps/*.jpg` and `vendor/<phase>/snaps/*.jpg`. Output: `COLOUR.md`
(summary per phase, per-patch table, grey row), `colour/chart_de.json` and a debug overlay PNG per stack and phase. The overlays
are camera pictures: keep them local, never publish them.

## Method

- Patch detection per frame: flat-region blobs, 6x4 lattice, homography, 8 orientations scored by grey-normalised dE00; the sample
  is the central 50 % of each patch. Patches with more than 2 % of pixels at >= 250 are flagged `*` (clipped) and left out of the
  "unclipped" means.
- sRGB -> linear -> XYZ D65 -> Bradford -> XYZ D50 -> Lab D50, CIEDE2000 against the reference.
- Exposure: one linear-light factor so that grey A4 matches the reference (AE target differences drop out, the colour cast stays).
  `gn` additionally removes the white balance with per-channel gains fitted on A2..A5: what remains is matrix, saturation and tone.
- Under a 3000 K lamp the camera white balance decides the cast against the D50 reference, so the fair stack comparison is
  open-vs-vendor dE00 (both stacks see the same light) and `gn`.

## Results 2026-11-12 (JPEG snapshots, 4 frames per stack; ch1 = sub stream, 8 frames)

dE00 mean over the 24 patches, open / vendor (gn = white balance removed; open-vs-vendor = dE00 between the two stacks).

| run | phase | open | vendor | gn open / vendor | open-vs-vendor mean (max) |
|---|---|---|---|---|---|
| Eufy T40 sc830ai, 3000 K | ch0 | 9.13 | 9.66 | 4.16 / 4.31 | 1.01 (2.4) |
| Eufy T40 sc830ai, 3000 K | ch1 | 10.56 | 9.43 | 4.38 / 4.15 | 2.00 (9.4, clipped) |
| Eufy T40 sc830ai, 5000 K | ch0 | 9.73 | 9.81 | 4.69 / 4.60 | 0.87 (2.1) |
| Secuplug T10 jxh42, 5000 K | ch0 | 9.74 | 9.65 | 7.53 / 7.47 | 0.35 (1.5) |
| Secuplug T10 jxh42, 5000 K | ch1 | 9.82 | 9.60 | 7.58 / 7.41 | 0.66 (1.7) |

Reading: ch0 is vendor-equal or better on all three runs (open-vs-vendor mean at or below 1 dE00, same tone and saturation). The
only clear gap is the T40 sub stream at 3000 K (+1.1 dE00 mean), where the whites clip in both stacks (A1..A3 code 244-254) and the
difference sits on clipped saturated patches (B3 yellow 31.9 vs 26.1, C6 21.6 vs 19.0, C1 18.2 vs 16.2); on unclipped patches
the open-vs-vendor mean is 0.88. Tracked as an issue with the patch numbers. Absolute dE00 of 9-10 on both stacks is the vendor tuning
(saturation 0.8-0.9 of the reference, light skin 12-15): a per-sensor CCM fit (the goal of #102) is a separate step and not done here.

## CCM option: fitted colour matrices for the T10/T20 (#102, beyond vendor, opt-in)

The vendor tuning stays the default. `OPENIMP_CCM_FILE=<file>` in the streamer's environment makes OpenIMP (T10/T20 builds)
replace the three colour matrices of the Apical tuning (calibration tables 40/41/42 = A / D40 / D50) at `IMP_ISP_EnableTuning`
through the stock table control 0x8000161 (retry at `SetISPRunningMode` if the ISP was not ready). The firmware keeps its own
light-source choice (AWB CT < 3426 K -> A, < 4501 K -> D40, else D50, 35-step blend), the saturation matrix and the day/night
handling; the day/night switch does not reload these tables. A module reload (or a streamer start without the variable after a
reload) brings the vendor matrices back. Log line per matrix: `[IMP-ISP] OPENIMP_CCM_FILE: D40 CCM written (EnableTuning)`.

File format (one line per light source, floats, 1.0 = unity, row-major RR RG RB / GR GG GB / BR BG BB, `#` comments, a missing
line keeps the vendor matrix):

```
A   1.3995 -0.1905 -0.3419 -0.2723 1.5425 -0.3161 -0.1573 -1.8979 3.3426
D40 1.2599 -0.2004 -0.1401 -0.3088 1.6222 -0.3281 -0.0912 -1.3599 2.5515
D50 1.4636 -0.3403 -0.1156 -0.2490 1.5526 -0.3353 -0.0814 -1.1972 2.3579
```

These are the jxh42 matrices of the Secuplug SP1 T10 ([tools/ccm/jxh42-t10.txt](../tools/ccm/jxh42-t10.txt)); vendor tables of
the jxh42 bin for comparison: A 1.406 -0.324 -0.082 / -0.219 1.309 -0.090 / -0.148 -1.105 2.254, D40 1.102 0 -0.102 / -0.344
1.602 -0.258 / -0.070 -1.051 2.121, D50 1.051 0 -0.051 / -0.227 1.648 -0.426 / -0.078 -1.000 2.078. Enable on a camera, e.g.:

```
cp jxh42-t10.txt /etc/openimp-ccm.txt
# in the streamer start (init script): OPENIMP_CCM_FILE=/etc/openimp-ccm.txt /usr/bin/timpsd -c /etc/timps.conf
```

Fit ([tools/ccm_fit.py](../tools/ccm_fit.py)): JPEG patches -> linear sRGB; a 3x3 C in linear light maps them to the reference
(objective: mean CIEDE2000 with kL = 2 over unclipped patches, white A1 left out, after the A4 exposure normalisation, small pull
to identity); C is scaled so the grey luminance stays (the AE lands on the same gain, matched exposure). White-balance residue is
folded into C (greys come out neutral); the T10 AWB statistics are taken before the CCM, so the AWB does not fight it (gains
unchanged on the device). New table = S^-1 C S T from the matrix the ISP ran (`ISP CCM ..` of `/proc/jz/isp/isp_info`, S = the
saturation matrix of the 'Saturation Target'). kL = 2 because the box light falls off towards chart rows C/D (both the T10 and the
T40 show rows C/D 10-15 L* darker on both stacks), which no matrix can fix; the standard dE00 (kL = 1) is reported as well.

Results 2026-11-16, T10 Secuplug SP1 jxh42 in the light box (KAJPLATS group, open stack OpenIMP next + open-tx-isp of the image,
auto AWB, day pinned, 4 JPEG frames per point; before = vendor tuning, after = `OPENIMP_CCM_FILE`, A refined once on the device;
vendor stack (vendor libimp + vendor module) at 3000 K 9.01, at 5000 K 9.53 dE00 from the 1116 box runs):

| lamp | AWB CT / CCM | dE00 mean before -> after | dE00 kL=2 | gn | C* ratio | hue err deg | skin dE00 | grey a*/b* | noise Y' std | total gain |
|---|---|---|---|---|---|---|---|---|---|---|
| 2700 K | 3200 / A | 10.97 -> **8.43** | 8.32 -> 4.99 | 8.69 -> 7.67 | 0.79 -> 0.96 | 12.4 -> 5.5 | 14.1 -> 14.0 | +3.1/+11.6 -> +1.6/-0.6 | 1.69 -> 1.70 | 523 -> 534 |
| 3000 K | 3500 / D40 | 8.95 -> **8.31** | 5.77 -> 4.87 | 7.70 -> 7.28 | 0.85 -> 0.93 | 5.3 -> 3.9 | 14.8 -> 14.5 | +1.5/+4.9 -> +0.0/+0.3 | 1.72 -> 1.64 | 534 -> 534 |
| 4000 K | 4200 / D40 | 8.87 -> **8.32** | 5.48 -> 4.89 | 7.47 -> 7.31 | 0.91 -> 0.99 | 5.6 -> 4.4 | 15.7 -> 15.2 | +0.9/+5.2 -> -0.7/+0.8 | 1.74 -> 1.72 | 512 -> 512 |
| 5000 K | 4900 / D50 | 9.51 -> **8.28** | 5.98 -> 4.75 | 7.75 -> 7.26 | 0.86 -> 0.95 | 6.1 -> 5.1 | 15.6 -> 14.8 | -1.3/+4.4 -> +1.9/+1.1 | 1.71 -> 1.68 | 918 -> 918 |
| 6500 K | 6300 / D50 | 9.68 -> **8.52** | 5.90 -> 4.60 | 7.79 -> 7.05 | 0.86 -> 0.97 | 5.5 -> 5.2 | 16.1 -> 15.2 | -0.9/+2.5 -> +1.0/+0.1 | 1.71 -> 1.75 | 861 -> 861 |

Reading: the option lowers the mean dE00 by 0.6-2.5 at every lamp (largest at 2700 K, where the vendor tuning leaves a strong warm
cast, grey b* +11.6), saturation goes from 0.79-0.91 to 0.93-0.99 of the reference (not above 1), hue error drops, greys become
neutral (|a*|, |b*| <= 1.9), noise in the grey patches and the AE gain stay unchanged. The remaining ~8.3 dE00 is mostly lightness
of chart rows C/D (box light fall-off, see above; skin 14-15 is lightness) and the tone curve, not the matrix. Night/IR is
monochrome and not affected. Fitted for one sensor and one camera; other sensors need their own fit (#102 stays open).

## T21: box colour calibration at runtime (`tools/tisp_nl`, #102, beyond vendor, test tool, opt-in)

The T21 has no CCM attribute in its IMP API (vendor 1.0.33 has no `IMP_ISP_Tuning_SetCCMAttr`), so the light-box calibration uses
the tisp tuning netlink channel that the vendor tuning tool talks to and that both the vendor `tx-isp-t21.ko` and open-tx-isp
`tx_isp_t21` serve (`tisp_param_operate_process`, protocol 23, replies to port 50; open-tx-isp `driver/t21/tx_isp_t21_recovered.c`
`tisp_netlink_init` / `tisp_ccm_param_array_set`). [tools/tisp_nl/tisp_nl.c](../tools/tisp_nl/tisp_nl.c) is a small static client:

| call | message | effect |
|---|---|---|
| `tisp_nl get 0x7d` / `0x7e` / `0x7f` | type 0 | A / T / D linear CCM, 9 x int32, 14-bit two's complement Q10 (1024 = 1.0, rows sum to 1024) |
| `tisp_nl get 0x81` / `0x82` | type 0 | saturation list per EV (9, 256 = 1.0) / CT thresholds of the CCM choice (jxf23: 4500 3100) |
| `tisp_nl set ID w0 ..` | type 1 | copies the array and recomputes the CCM (`force_update`, `jz_isp_ccm`) |
| `tisp_nl wbget` / `wbset MODE R B` | type 3 / 2, id 5 | WB attr {mode, manual r/b, statis r/b, global r/b}; mode 1 = manual gains (256 = 1.0), 0 = auto. `statis` / `global` are the AWB's R/G and B/G RATIOS in /256, not gains: the manual gain that reproduces the auto picture is 65536 / global (the isp proc file prints `0x10000 / global` as "ISP WB weighted rgain"); jxf23 in the box: global 221 / 101 at 3000 K = gains 297 / 649, 156 / 131 at 5000 K = 420 / 500 |

Changes last until the ISP reloads the bin tables (module reload; the day/night parameter switch reloads the CCM tables). jxf23
(Victure PC420 T21) vendor tables: A 1.484 -0.367 -0.117 / -0.344 1.602 -0.256 / -0.359 -1.672 3.030 (bin row sum 1023),
T 1.594 -0.451 -0.139 / -0.355 1.572 -0.219 / -0.210 -1.051 2.260, D 1.613 -0.651 0.039 / -0.427 1.781 -0.355 / -0.131 -0.980 2.113;
saturation list 210 210 200 200 200 200 180 95 90 (the vendor desaturates to 0.82 x even in bright light: chroma ratio 0.78 in the box).

QEMU (-M ingenic-t21, t21b24 image, 2026-11-23): get of all arrays = the bin values, set + read-back, manual WB, frames continue
(snapshot 200 before / during / after), restore by writing the saved arrays back, no oops.  The light-box runner phase `boxcal`
(runner `lb_boxcal.py`) fits the matrix and the WB gains on the chart per lamp CT, applies them through `tisp_nl`, captures the chart
again and restores the vendor tables; the report shows vendor / open / open + calibration as "info: beyond vendor" next to the gate
rows (the gate compares vendor and open without the calibration).

Saturation list: vendor `cm_control` builds `sat/256 x I + (1 - sat/256) x 1 x (0.299 0.587 0.114)` (Q16 19595 / 38470 / 7471) and
the effective colour matrix is CCM @ that matrix, so 256 is exactly "no saturation change".

First device run (T21 jxf23 in the box, 2026-11-24) and what it changed: the runner had written the `global` ratio words as manual
gains (green picture) and fitted at the normal AE with 12-15 of 24 patches clipped; the calibrated picture was worse (dE00 11.7 ->
26.2 at 3000 K, 11.0 -> 19.6 at 5000 K). The runner now captures the chart at a lower exposure (`image.ae_compensation`), uses an
identity-CCM base with the AWB's own gains, refuses a fit with fewer than 18 usable patches or out-of-range gains / coefficients,
applies a result only if its leave-one-out predicted dE00 beats the normal picture, and keeps it only if the chart measured with it
is better (otherwise the report says "rejected" and shows the vendor tuning). Offline on the stored low-exposure shots the jxf23
vendor matrix is already close to the dE00 optimum: about 75 % of the 8-9 dE00 is lightness (tone curve / ADR, grey row dL -29 .. +8),
the best matrix gains 0.2-0.45 in-sample and nothing leave-one-out, so "rejected: no predicted gain" is the expected verdict on
this sensor until a device run shows otherwise. Build: `mipsel-linux-gcc -Os -static -o tisp_nl tools/tisp_nl/tisp_nl.c`
(thingino T21 toolchain). Only one netlink tuning client at a time (fixed port 50).
