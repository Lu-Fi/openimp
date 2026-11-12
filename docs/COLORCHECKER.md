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
