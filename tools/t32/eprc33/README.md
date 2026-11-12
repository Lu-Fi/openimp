# T33 eprc port tooling (#295, WIP)

Development aids for porting the vendor libimp 2.2.0 "eprc T33" rate control (H.264 first).
Nothing from the vendor library is stored here; every tool reads the library / Ghidra project you pass.

- `ghidra/SwitchFix.java` (+ `sw.py`): resolves the gp-relative PIC switch tables Ghidra misses.
- `ghidra/NoRet.java` (+ `nr.py`): marks `__assert`/`exit`/`abort` calls no-return (fixes merged function bodies).
- `ghidra/CommitParams.java`, `ghidra/SetSig.java` + `sig264.txt`: commit / override prototypes (FP-register
  mis-detection on FRAME_START, get_P_frame_qp, estimate_qp).
- `patch264.py`: call-site fixes Ghidra loses (stack arguments of the indirect adjust_P_frame_dqp calls).
- `g2c.py`: Ghidra pseudo-C -> C on a 32-bit OEM address arena (`e33rt.h` accessors).
- `diff.py` / `rand.py`: per-call differential test (vendor under unicorn vs the C build, memory compared byte for
  byte over E, the controller block B and the library globals). Needs the oracle slice block (Hh+0xf9c) and its
  +0x250 map, which `tools/eprc_t33_oracle.py` does not set yet.
- `rand3.py` (+ `rand.py`, `rand_rate.py`; `reg.sh SO OUT` runs all 96): scenarios that reach every I-QP branch:
  GOP 1, no initial QP, 5..60 fps, run-time bit-rate changes, SMART/AVBR still-scene knobs (+0x25c, +0x260 still map,
  +0x26c/+0x26d, +0x168, +0x264) and the HERA complexity sum (regs 43/44). A scenario passes only with 0 differing calls.
- `rand4.py` (in `reg.sh`, 144 scenarios in all): FRAME_START / FRAME_END / VIDEO_CFG branches: fixed QP, forced IDR
  periods that differ from the GOP, small still-map blocks and class counts, ROI regions, intra refresh and the
  per-picture motion knobs (X+0x151.., X+0x164..), out-of-range configuration values. Inputs that i264e would not
  produce are poked at function entry (`Harness.poke`, before the snapshot), so both sides still get the same input.
- `rand5.py` (in `reg.sh`, 176 scenarios in all): the slice block (VPI_set_sliceinfo, get_bu_target_bits): HW revision
  e+0x34 (6 / 7), intra-refresh ring and second reference (cur+0x7b0..0x7b8), SMART bit map (cur+0x43c), buffer
  addresses (cur+0x44c..0x470), last IDR slice sizes (cur+0x21c / 0x228.. / 0x328), the work-buffer flag (vb+0xa4).
  Coverage: build the swap oracle with `-O0 --coverage` and run gcov on `eprc_t33_h264.c`; compare results only with the
  `-O1` build (the generated C has uninitialised locals in the P path that differ at -O0).
- `rand6.py` (group A in `reg.sh`, 208 scenarios in all): the SMART rule / level and motion-step table rows of the
  slice block. Group A keeps the inputs in the ranges the vendor encoder produces (rule 0..2 / level 0..3 through
  P+0x26c/0x26d with the still map on, out-of-range values at VIDEO_CFG, motion mode cur+0x7d4 0..71): 0 differing
  calls. Group B (`rand6.py SEED N K0 VB B`, exploratory) goes past the copied .data and differs by design.

## H.265 (eprc_default_set_T33, JZ_VPU_RC_*_T33)

- `../../eprc_t33_h265_oracle.py`: the vendor H.265 controller under unicorn through the vendor glue
  (IMP_Encoder_CreateChn up to i265e_init for the parameters, I265e_configure, I265eRatectrl_rcInit /
  _rcStart / _rcEnd with a synthetic picture, slice block, maps and VPU buffers); `--scenarios` 'S' + 'F' lines.
- Ghidra (project with the 2.2.0 .so): `ghidra/SetSig.java ghidra/sig265.txt 10000`,
  `ghidra/CommitParams.java 1ad550 1c1db0`, `ghidra/SwitchFixRa.java ghidra/swra265.txt 10000` (the two jump
  tables dispatched through `jr ra`, which `sw.py` skips and Ghidra takes for returns), then
  `ghidra/DecompRange.java OUT 1ad550 1c1db0`.
- `patch265.py`: the three indirect adjust_P_frame_dqp calls in estimate_qp (stack arguments lost).
- `diff265.py SEED N [K0 [VB]]`: per-call differential test (E = handle + 0x2088); `reg265.sh SO OUT [N]`
  runs N scenarios (seed 1 synthetic statistics, seed 2 random statistics and per-row IDR sizes), seed 3 random
  rate-control attributes) and the poke scenarios of `rand265p.py` (5 N / 2 in all).
- `rand265p.py SEED N [K0 [VB]]`: the H.264 rand4 / rand5 knobs on the H.265 layout, poked at function entry:
  out-of-range configuration values at VIDEO_CFG, HW revision e+0x34 (6 / 7), size-based reference QP
  (inst+0x54), SMART motion level inst+0x7f / rule inst+0x7d, the per-picture flags e+0x30 / e+0x53,
  st+0x398 back to 0 at FRAME_END, IDR periods that differ from the GOP, GOP 2.
  Seeds 3 / 4 add FRAME_START knobs (own random stream, seeds 1 / 2 unchanged): FIXQP (mode 0), the still map
  (cur+0x320..0x327: block size 8..32, 2..4 classes as the 4 counters at ms+0x2c allow, hysteresis), motion mode
  cur+0x340 / 0x341, forced intra cur+0x342 / 0x344 and intra refresh e+0x35d with cur+0x32a..0x338.
  `reg265.sh` runs seeds 1..4 (7 N / 2 scenarios).
- `swap265.py` (`CFLAGS_X='-O0 --coverage' OUT=...` for a gcov build; in the local work directory with h265p.c, got.txt, t265.c, e265oem.h): the generated
  controller with the functions of `swapmap265.txt` taken from `src/eprc/eprc_t33_h265.c`.
