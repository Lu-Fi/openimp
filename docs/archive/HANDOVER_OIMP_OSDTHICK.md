# Handover: OpenIMP OSD — uniform stroke thickness for diagonals

Status: 2026-10-03 — **fully implemented, host-tested, 3 local commits — NOT pushed.**
**One point is deliberately open: nothing has been checked on the device** (section 6).

This version contains the improvements from the review: `lw = 1` is now byte-identical to
stock (end point p1 excepted), the complete angle sweep is included as a fixed host test,
and the statements about the step count and the caps are corrected (section 4.2).

Branch `claude/osd-thick-lines`, worktree `<path>/openimp-osdthick`.

---

## 1. Task (original, verbatim)

> Task (OpenIMP, host work only, no camera): OSD lines with uniform thickness.
> Repo: <path>/openimp. Base: origin/claude/openimp-all-10 (4c1ffe1). Create your own
> worktree and a new branch claude/osd-thick-lines. Commit locally, do NOT push.
>
> Requirements: uniform stroke thickness for lines with an angle ≠ 0°/90° (Bresenham with
> perpendicular span, or Murphy's thick line), integer arithmetic only, no floats, no
> malloc in the drawing path; keep clipping and NV12 Y/UV handling (UV half resolution);
> the dry pass (`cv.dry = 1`) must still cover all rows actually written;
> extend host tests in `tests/t31` (`make -C tests/t31 check`): perpendicular thickness of
> 45°/30°/60° lines at lw = 1,2,4,8 within ±1 pixel of the target, horizontal/vertical lines
> and rectangles byte-identical to the old result, no writes outside the frame,
> dry-pass rows ⊇ written rows; `tests/t30 check`, `tests/t23 check` must stay
> green.
>
> Deliverable: short report with commit hash, changed files, test output and a
> host micro-benchmark (old vs. new, 1080p diagonal lw=4).
> "No changes outside the branch, no camera, no push."

All rules were observed: committed only locally, pushed nothing, touched no other branch/worktree,
no camera access, no floats and no `malloc` in the drawing path.

---

## 2. Result at a glance

| | |
|---|---|
| Branch | `claude/osd-thick-lines` |
| Base | `origin/claude/openimp-all-10` = `4c1ffe1` |
| Commits | **3 commits on top of the base, not pushed**: `cb272dd` (implementation, updated after the review) + two docs commits (this file, `T31_OSD_RE.md` §3.6) — exact SHAs: `git --no-pager log --oneline 4c1ffe1..HEAD` |
| Diff | 7 files, **+1,258 / −132** |
| Host tests | `tests/t31` (incl. `osd_draw_test: ok`), `tests/t30`, `tests/t23` — all exit 0 |
| Benchmark (host, 1080p diagonal, lw 4) | old **53.9 µs / 10,908 Y bytes**, new **23.4 µs / 9,591 Y bytes** = **0.43×**; dry+draw 69.0 → 34.6 µs |
| Measured thickness (perpendicular) | lw 1/2/4/8 at 45°: 0.71 / 2.12 / **4.24** / **7.78** px (old: 0.71 / 2.12 / 4.95 / 10.61); angle sweep 0…180° lw 2…8: max. deviation **0.463 px** |
| Byte identity | axis lines, rectangles **and `lw = 1`** (any direction) |
| Affected targets | T31, T20, T21, T30 (shared header; T23 compiles the OSD block out, T40/T41 do not compile the file) |
| Open | **no device check** of the diagonal and of the flush band (section 6) |

---

## 3. Commits and files

`git --no-pager log --oneline 4c1ffe1..HEAD` (3 commits, exact SHAs there):

```
docs: note the OSD diagonal stroke deviation in T31_OSD_RE.md
    docs/T31_OSD_RE.md               +10

docs: handover notes for the OSD thick-line change
    HANDOVER_OIMP_OSDTHICK.md        +282 (new)

cb272dd osd: uniform stroke thickness for diagonal lines
    src/t31/openimp_t31_osd_draw.h   +205 / −74
    tests/t31/osd_draw_test.c        +500 / −55
    tests/t31/osd_draw_old.h         +120 (new)
    tests/t31/osd_thick_bench.c      +125 (new)
    tests/t31/Makefile               +16 / −3
```

(Note: `git commit --amend`/rebase changes SHAs — the SHAs in this document are hints,
authoritative is `git --no-pager log --oneline 4c1ffe1..HEAD`.)

- **`src/t31/openimp_t31_osd_draw.h`** — the shared, header-only CPU drawer for
  `OSD_REG_LINE`, `OSD_REG_RECT`, `OSD_REG_BITMAP` on NV12. Hardware-free, host-testable.
  Only production consumer: `src/t31/openimp_t31_services.c` (line 25).
- **`tests/t31/osd_draw_old.h`** (new) — **frozen copy** of the stock drawer at the
  base commit (`osd_draw_line_old`, `osd_draw_rect_old`, `osd_fill_new_old`). Serves as the
  byte-identity reference, as the "before" column of the thickness table and as the benchmark opponent.
- **`tests/t31/osd_draw_test.c`** — `lw = 1` identity test, angle sweep, axis identity test,
  thickness table, diagonal clip and dry test. The old `test_line_reference()` (which fixed the
  stock staircase geometry) is replaced. The test links `-lm` (`cos/sin/lround` only
  to generate the end points; the drawer itself stays float-free).
- **`tests/t31/osd_thick_bench.c`** (new) + `make -C tests/t31 bench` — host micro-benchmark.

---

## 4. What was done technically

### 4.1 The problem in one formula

The stock drawer draws a line as a sequence of `lw × lw` stamps, one per step of the
dominant axis. For x-dominant, one step covers `lw + (lw − 1)·tan(θ)` rows,
so across the line `lw·cos θ + (lw − 1)·sin θ`. At 45° this is exactly `2·lw − 1` rows
= `(2·lw − 1)/√2 ≈ 1.41·lw` pixels. The line is thus **never** `lw` wide, but always
wider — up to a factor 1.41 at large `lw`.

### 4.2 The new band

For diagonals (`dx ≠ 0` **and** `dy ≠ 0`) `osd_draw_diagonal()` walks along the dominant axis
and fills **one** run along the minor axis per step:

```
t = round(lw · hypot / major)          /* run length in pixels */
r0 = c − t/2;  r1 = r0 + t − 1
```

`hypot` comes from `osd_isqrt64(((uint64_t)adx*adx + ady*ady) << 20)` (hypotenuse in
1/1024 units), the rounding term is `(lw*hq + (major << 9)) / (major << 10)`; `t` is clamped to
`[1, OSD_DRAW_MAX_LW]`. Projected onto the line normal, the band is thus `lw` wide
— within half a pixel length, and for an integer run this is the best that is
possible (measured worst case in the angle sweep: **0.463 px**). The minor axis is computed as
`floor(slope · s)` and clamped to the end-point range, both ends are extended by `lw/2`.

**Two restrictions, honestly:**
- The caps are **not** perpendicular but parallelogram corners (as with the stock stamps):
  the overhang runs along the dominant axis, so projected onto the normal it is at most
  `lw/2 + 0.5` pixels. The band is exactly `lw` wide, not the end face.
- The **step count is equal to that of the stock drawer only on the inside**. Because of the overhang, up to `lw/2` steps are added at
  both ends, so up to `lw + 1` together. The statement "same step count"
  applies to the inner steps — and exactly these are measured by the test.

**Properties required by the task:**
- integer only: no `float`/`double` anywhere; the only division is the scaling of `t`
  (once per line, not per pixel),
- no `malloc`, no static state, no allocation,
- one pass, one run per step, every pixel of the band exactly once,
- clipping inline, NV12 semantics unchanged (`osd_fill_col()` writes Y per row and the
  UV pair at `(x & ~1)` in chroma row `row >> 1`),
- `dry` respected: `osd_fill_col()` / `osd_fill_box()` update `ymin`/`ymax` and
  write not a single byte when `c->dry` is set.

### 4.3 Axis lines and `lw = 1` stay byte-identical

`adx == 0 || ady == 0` keeps **exactly the stock path**: `adx < 2*lw` → one filled box around the
centre x, otherwise the union of the stamps at `pmin..pmax`. Thus horizontal and
vertical lines — and therefore all rectangles made of four of them — are **byte-identical**
to the old result.

`lw == 1` too: a single pixel has no width to distribute, and the overhang of the
band would add a step at the end point p1. Therefore for `lw == 1` the
stock dispatch continues to apply (`|dx| < 2*lw` ⟺ `|dx| ≤ 1` → box) and in `osd_draw_diagonal()` the
stock step limit **p0 inclusive, p1 exclusive**. A one-pixel stroke is thus
byte-identical in every direction. Only real diagonals with `lw ≥ 2` take the new path.

The old helper `osd_fill_new()` (incremental stamp union, used only by the old path)
thereby became dead code and was removed; its frozen copy lives in the test.

### 4.4 Deliberate behaviour change

`|dx| < 2*lw` was dropped as the dispatch condition for the box path **for `lw ≥ 2`** (it
now applies there only at `adx == 0`); for `lw == 1` it is kept, because there it is
byte-identical and costs nothing. A **short diagonal** with `lw ≥ 2` used to be a box and is now
a band. This is intended and matches the goal of the task.

---

## 5. Verification (host)

### 5.1 Test runs

```
make -C tests/t31 check   # osd_draw_test: ok   (exit 0, incl. t31-p2-jpeg-source-test)
                          # angle sweep 0..180 deg, lw 2..8: worst |width - lw| = 0.463 px
make -C tests/t30 check   # System Init/Exit tests passed      (exit 0)
make -C tests/t23 check   # T23 JPEG user tables: all checks passed   (exit 0)
```

New in the OSD test:

| Test | Checks |
|---|---|
| `test_axis_identity` | 600 deterministic LCG cases, per case one rectangle **plus** one line (even indices horizontal, odd vertical, so that clamp and `offPos` do not turn them into a diagonal): coordinates −16…79 / −16…63, `offPos` −4…4, `lw ∈ {1,2,3,4,7,9,100,0xffffffff}`; `memcmp` against the frozen stock drawer **and** the same `ymin/ymax` band |
| `test_lw1_identity` | 15 fixed cases (corners, `|dx| = 1/2` → box path, 45°, far outside, `INT_MIN/INT_MAX`) × 3 `offPos` plus 400 random diagonals (LCG, with `dx ≠ 0` **and** `dy ≠ 0`): `lw = 1` must be `memcmp`-equal to the stock drawer, including the band |
| `test_diagonal_thickness` | 45°/30°/60° at lw 1/2/4/8: perpendicular thickness within ±1 px, exactly one connected run per step, same inner step count as old, no pixel further than `lw/2 + 1.5` from the line, area/length = width |
| `test_angle_sweep` | **0…180° in 1° steps × lw 2…8** (1,267 runs, segment 200 px in the middle of a 512×512 frame): every inner step exactly one run, no hole, perpendicular width within `lw ± 0.5`; the worst case is printed (**0.463 px**) |
| `test_diagonal_clip_dry` | 14 segments (negative coordinates, all four directions, `INT_MIN`/`INT_MAX`, `lw = 0xffffffff`): edge bytes and row padding untouched, dry pass writes nothing, and **every row with written pixels lies in `[ymin, ymax]` of the dry pass** |

### 5.2 Thickness table (perpendicular, inner steps only)

```
  45 deg lw 1: new 0.71   old  0.71
  45 deg lw 2: new 2.12   old  2.12
  45 deg lw 4: new 4.24   old  4.95
  45 deg lw 8: new 7.78   old 10.61
  30 deg lw 1: new 0.87   old  0.87
  30 deg lw 2: new 1.73   old  1.73..2.60 (mean 2.23)
  30 deg lw 4: new 4.33   old  4.33..5.20 (mean 4.96)
  30 deg lw 8: new 7.80   old 10.40..11.26 (mean 10.43)
  60 deg lw 1: new 0.87   old  0.87
  60 deg lw 2: new 1.73   old  1.73..2.60 (mean 2.23)
  60 deg lw 4: new 4.33   old  4.33..5.19 (mean 4.96)
  60 deg lw 8: new 7.79   old 10.39..11.26 (mean 10.43)
```

At `lw = 1` the geometry is identical: `2·lw − 1 = 1` row at 45° is
`1/√2 ≈ 0.71 px`; there is nothing to improve. For `lw = 1` OpenIMP continues to draw the
stock path, this is secured by `memcmp` against the frozen stock drawer
(`test_lw1_identity`); only the end point p1 is left out as in stock.

### 5.3 Micro-benchmark (host, `make -C tests/t31 bench`)

1920×1080, diagonal `(0,0)–(1919,1079)`, `lw = 4`, 200 iterations per run:

```
  old staircase  one pass :     53.9 us    10908 Y bytes  1080 rows
  new band       one pass :     23.4 us     9591 Y bytes  1080 rows
  old staircase  dry+draw :     69.0 us
  new band       dry+draw :     34.6 us
  ratio band/staircase    :     0.43 one pass, 0.50 dry+draw
```

The new band is thus about **2.3× faster** than the stock stamp and writes **12 %**
fewer Y bytes. Figures are host `-O2` without target flags: order of magnitude, not a device figure.
Control run: 53.2 / 23.7 µs, dry+draw 68.9 / 31.8 µs — the scatter is about ±10 %.

### 5.4 Production TU

```
cc -fsyntax-only -std=gnu99 -O2 -Wall -Wextra -Isrc -Iinclude src/t31/openimp_t31_services.c
# clean
```

(With `-std=c99` it fails on a pre-existing, unrelated `O_CLOEXEC` problem of the
file, not on this change.)

---

## 6. ⚠️ Open point: device check missing, and the deviation is deliberate

**As instructed, work was done exclusively on the host.** The path
`openimp_t31_osd_apply()` (dry pass → flush → draw → flush) is only host-tested. On the
first device run, check:

1. A diagonal is visibly **continuous, of equal thickness and without gaps** (at lw ≥ 2).
2. The flush band still covers the band completely (no cut-off edge at top/bottom).
3. When the angle changes, the stroke no longer looks "thicker" but constant.
4. A line with `lw = 1` looks as before (byte-identical, only host-verified).

**There is no device test for this yet:** `docs/T31_OSD_IPU_TEST.md` covers only
bitmap and cover layers; there is no case for LINE/RECT geometry. Such a case
would have to be created.

**The visible change is intended, but it is a deviation from stock:** Diagonals are drawn
**thinner** than before. Existing OSD layouts with diagonals look different afterwards.
If "byte-identical to the stock OSD" is a hard project criterion, this commit
needs an explicit sign-off. Axis lines, rectangles and `lw = 1` stay unchanged
(byte-identical).

**Honest correction of an earlier assumption:** The widespread assumption that the stock stroke is
too thin by √2 at 45° is **wrong** — it is too **wide** (factor up to 1.41, see 4.1). The
measurements in 5.2 are the evidence. This correction is also a comment in the header and in the test.

---

## 7. Limits of the evidence

- The width is measured over inner steps (at least `lw + 4` pixels from the end), not at
  the caps. The caps are parallelogram corners and up to `lw/2 + 0.5` px beyond the normal
  — deliberately so, see 4.2.
- The angle sweep is now complete (0…180° in 1° steps for `lw 2…8`), but the
  thickness check applies only to the inner steps; the end-point geometry is not checked 1:1 against
  stock (only `lw = 1` is byte-identical everywhere).
- `lw = 0` is covered only by the guard (`lw <= 0` → immediate return), not tested.
- The benchmark is host `-O2` without target flags; it shows the order of magnitude, not the
  device run time.
- Byte identity is checked against the **frozen** stock drawer from `4c1ffe1`, not
  against device recordings of the stock `libimp`.

---

## 8. Rules (state)

- **No push.** The branch is local, `git status` clean.
- **No other branch/worktree** touched (`git worktree list` unchanged).
- **No camera** used.
- Changes exclusively on `claude/osd-thick-lines`.
