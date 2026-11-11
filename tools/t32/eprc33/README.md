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
  Coverage: build the swap oracle with `-O0 --coverage` and run gcov on `eprc_t33_h264.c`; compare results only with the
  `-O1` build (the generated C has uninitialised locals in the P path that differ at -O0).
