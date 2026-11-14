# T31 hardware JPEG: addendum to the RE report and review of the implementation

> **Status (2026-11-14):** Hardware JPEG is now the default, `OPENIMP_T31_HW_JPEG=0` selects software JPEG (`src/t40/openimp_p2_encoder.c:681`, `src/t40/codec-t40.c:11262`); the companion stage is always off while hardware JPEG is on (`src/t40/codec-t40.c:11794-11809`). The review findings B1-B9 below are the state of 2026-09-29 and historical.

Status: 2026-09-29. Refers to `docs/T31_HW_JPEG_RE.md` and the branch
`claude/t31-hw-jpeg` (HEAD `01e7eff`) in the fork `Lu-Fi/openimp`. Research only: No
repository files were changed.

Sources:

- HLIL `libimp.so_hlil.txt` (`L<line>` / `0x<address>`)
- stock trace `stock_logs.txt` (`T<line>`)
- kernel `avpu/t31/*.c`
- code (`file:line`)

(`libimp.so_hlil.txt` and `stock_logs.txt` are local files and are not in the repository.)

Marking as in the report: **[V]** evidenced, **[I]** inferred, **[?]** open.

Note on the trace: According to `logread` in the same log, **rvd/Raptor** (IMP-1.1.6, T31-X,
JPEG q75 1 fps) was running, not prudynt. The trace consists of concatenated
`dmesg` outputs (T755, T1764, T2792). Lines are missing at these places. Because of this, e.g.
the job at T752 seemingly has no completion. This is an artefact of the recording, not a
lost IRQ.

---

## 0. Short conclusion

**Newly evidenced:**

- **Field names in words 0 and 2** [V]. `AL_Dump_TEncChanParam` prints the fields with names
  (L46102–46111, `0x41180–0x411e0`):
  - `+0x5d` = `LossLess` → bit 9
  - `+0x5e` = `RestartInterval` → [31:16]
  - `+0x60` = `AspectRatioUnit` → [13:12]
  - `+0x62/+0x64` = `X_Density`/`Y_Density` → word 2
  - `iInitialQP` = `tRCParam+0x18` = ChParam+0x80 (L46027) → JPEG quality
- **`0x8438` bit 1 = stream overflow** [V]. Error code `0x88` means, according to
  `AL_Encoder_ErrorToString` (L46221, case `0x41888`), "Stream Error: Stream overflow". The
  status field `Status+0x01` is the same field that carries the
  "buffer full" flag in the AVC path (`0x6d9a4`).
- **EP1 is byte-identical to the vendor** [V]. I extracted the vendor tables from the
  HLIL (`ZIGZAG_SCAN`, quant, `HUFFMAN_TABLE_*` at `0xe2fc0–0xe35f0`) and
  re-simulated `JpegTables_InitQuant`/`InitHuffman`. `HW_Encoder_BuildJpegEp1()` produces
  **exactly the same 0x790 bytes for q = 1…100**. The vendor's codebooks are
  **constants from `.rodata`** that are copied by `memcpy`. So the "next symbol" field is
  fixed and is not computed.
- **Header question** [I, very strong]:
  - In the whole `libimp` there is no JPEG marker constant (`FFD8/FFDB/FFC4/FFE0/FFDA/FFD9`)
    and no string "JFIF".
  - The hardware gets density, unit and restart interval. These are needed only for
    APP0/DRI.
  - EP1 contains DQT bytes and BITS/HUFFVAL, which are needed only for DQT/DHT.
- **The JPEG clock stays on permanently** [V]. `Process()` gates off only cores `< numCore`,
  so never the JPEG core (L66300–66357). This explains why `0x85F4` is written only once in the trace
  (T146).
- **In the vendor, JPEG and AVC share the stream buffers and the source frame** [V].
  `0x841C` ∈ {`0x06f75300`, `0x06e8d700`} (1080p) and {`0x0715d000`, `0x07135600`}
  (360p) are exactly the AVC `CL[048]` buffers (T131). The size `0xe7680` is therefore
  the size of the AVC stream buffer and not a JPEG formula.
- **No additional register accesses** [V]. All 34 JPEG jobs in the trace have the same
  sequence. The stock libimp does not read `0x85F8`, does not gate off the clock, writes
  no ack and never clears the mask.

**The most important findings in the implementation** (details in part B):

1. **B1:** Without an AVC IRQ thread there is no completion.
   - At start the JPEG thread can get the encode lock before the first AVC `Process()`.
     Likewise with pure JPEG or after the teardown of the host AVC channel.
   - Consequence: timeout, and HW JPEG is switched off for the rest of the process.
2. **B2:** `pthread_cond_timedwait` runs with `CLOCK_REALTIME`. The NTP jump at boot
   (in the log: "time disparity of 3298 minutes") produces a spurious timeout and switches
   HW JPEG off permanently.
3. **B3:** The buffers are sized globally for the resolution of the first JPEG frame.
   With rvd, if 360p comes first, 1080p then runs permanently in software, and there is a log entry
   for every frame.
4. **B4:** The quality is fixed at 75. `iInitialQP` is ignored, and the P2 layer
   loses a quality > 51 anyway.
5. **B5–B9:** Smaller points:
   - EP1 remainder is not zeroed
   - chroma padding rows are missing in the source buffer
   - cache direction `dir=2` instead of the proven `dir=0`
   - EOI check is too strict
   - teardown can shoot down a running JPEG job

---

## Part A – Open points

### A1. Command word 0 (`0x8400`), `0x85F0`, `0x85E4`, status registers

**Word 0 – complete bit map** (`JpegParamToCtrlRegs` L73828, `0x6f5f4–0x6f6a0`).
The fields of the local parameter block are filled in `SetJpegParam` (`0x6b3ec–0x6b458`):

| Bits | Source in the param block | Origin | Meaning | Status |
|---|---|---|---|---|
| [1:0] | `u32 @+8` | `ePicFormat>>8 & 0xF` | ChromaMode 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2 (assert `0x6b5c8`) | [V] |
| [5:4] | `u8 @+5` & 3 | `ChromaMode ? 3 : 1` (`0x6b450–0x6b458`) | number of components (SOF Nf / SOS Ns) | [V] value, [I] name |
| [8] | `u8 @+0` | **constant 1** (`0x6b438`, `var_38.b = 1`) | unknown | [V] always 1, [?] meaning |
| [9] | `u8 @+1` | ChParam+0x5d = **`LossLess`** (L46102) | lossless JPEG | [V] name |
| [13:12] | `u8 @+4` | ChParam+0x60 = **`AspectRatioUnit`** (L46104) | JFIF "units" | [V] name, [I] JFIF field |
| [31:16] | `u16 @+2` | ChParam+0x5e = **`RestartInterval`** (L46103) | DRI / RSTn | [V] name |

Further points on words 0 to 3:

- **Bit 8** has no source apart from the constant. `CtrlRegsToJpegParam` (L73857) also
  decodes it back only for traces.
  - The HLIL shows no semantics.
  - Candidates [?]: "generate header", "start of frame"/"first segment",
    "standard tables".
  - Check on the device: run the probe with word 0 = `0x031` (suggestion: option
    `-c <word0>` in `tools/t31_hwjpeg_probe.c`) and compare whether SOI/DQT/DHT
    are missing afterwards.
- The remaining bits of word 0 (2–3, 6–7, 10–11, 14–15) are **always 0** [V]. The mask
  `0xCFFF` in `0x6f6a0` also clears [15:12].
- **Word 2:** `X_Density<<16 | Y_Density` (L46105/46106). The values come from
  `setDensity` (L51629) with the aspect enum `Settings+0xFC`:
  - 4 (default) → 1:1
  - 2 → 4:3
  - 0/3 → 16:9

  [V]
- **Word 3:** Pitch as **u16** in [15:0], [31:16] is retained and is 0 after the
  memset (`0x6f6ac`). A pitch > 65535 is therefore not representable. [V]

**`0x85F0 = 1` before every job** (`AL_EncCore_EncodeJpeg` `0x6ce30`, trace at each of the 34
jobs directly before the zone):

- It is the same register as `0x83F0` (`ResetCore` `0x6c398`, AVC: 1, 2, 4
  one after another) in the window of core 1.
- For JPEG only bit 0 is pulsed. A 0 is never written, and nothing is waited for
  or read back.
- ⇒ Self-clearing soft reset/clear of the (only) JPEG pipeline before
  parameterisation. [V sequence; I semantics, strong by analogy with `0x83F0`]

**`0x85E4 = 1`** (`0x6ce80`):

- Analogous to `0x83E4`: AVC writes 2 (Enc1) or 8 (Enc2) there as CL push
  (`0x6cd18/0x6cdc0`).
- For the JPEG core this is bit 0 and thus a "start from registers", without a command list.
  `0x85E0` (CL_ADDR) is never written. [V value, I semantics]

**`0x85F8`:**

- Is **never read** by the vendor for JPEG [V]. `IsEnc1/2AlreadyRunning` (`0x6c678`,
  `0x6c6e8`) check only `(core<<9)+0x83F8` from the AVC path `AL_EncCore_Encode1/2`.
- In the trace each JPEG completion has exactly three RD ioctls, which corresponds to the three
  status reads (T193–T198). Before the start there are only the RMW reads for `0x8014`/`0x85F4`.
- Meaning for JPEG [?]. Probably bit 1 = busy, as with AVC. Read only for diagnosis.

**Status `0x8430`** (→ `Status+0x70`, `0x6d388`):

- Otherwise written back only by `JpegStatusToStatusRegs` (L73244) for the trace dump.
- `+0x70` is behind all fields of the AVC SliceStatus (`EncodingStatusRegsToSliceStatus`
  L73068 fills at most `+0x6c`). So it is a JPEG-specific field without a consumer.
- Meaning [?]. Hypotheses: clock cycles, bit counter, MCU counter.
- Check on the device: note the value for 1080p/360p and q50/q90 and correlate it with run time or
  length.

**Status `0x8434`:** Length in bytes. It is taken over **unmasked** (AVC masks
`& 0x3fffffff`, `0x6d978`). [V]

**Status `0x8438`:**

- Only bit 1 is evaluated: `(v>>1)&1` → `Status+0x01` (`0x6d3e4`) → error `0x88` =
  **"Stream Error: Stream overflow"** (L46221 ff.) [V].
- The completion including the IRQ also comes on overflow. The error path exists only in
  `EndJpegEncoding`, i.e. after the IRQ. [V path, I hardware behaviour]
- All other bits [?].

### A2. EP1 details

**Quant bytes:** Byte-wise `sb` stores (`*($a3_1-1) = $s3_1 & 0xff`, `0x4b538`/`0x4b584`),
zigzag index k at EP1+k or EP1+0x40+k [V]. There is no packing into words. The
byte order is thus address order. The BITS arrays, by contrast, are stored as u32 with
MSB-first content (`0x4b690–0x4b70c`). The difference is real and is reproduced
correctly in the implementation. [V]

**Further evidence on quant and reciprocals:**

- The clamps are confirmed [V]:
  - `v == 0` → 1 (`0x4b584`)
  - `v ≥ 256` → `0xFF` (`0x4b578`)
  - `S &= 0xFFFF`
  - `Q == 0` → trap (`0x4b4c8`)
- Reciprocals: `(int16)((0x10000 + (q>>1)) / q) − 1`, stored as u16 at `2·ZIGZAG_SCAN[k]`,
  i.e. in raster order (`0x4b5e0–0x4b608`) [V].

**`+0x180 = 1`, `+0x184 = 0`:**

- Both are constants, independent of table content, quality and colour mode [V]:
  - colour: `0x4b6f0/0x4b6f4`
  - mono: `+0xC0/+0xC4` (`0x4b808/0x4b868`)
  - lossless: `+0x00/+0x04` (`0x4b7c4/0x4b7e4`)
- Hypothesis [I, weak]: Reading the word LE byte by byte gives
  `[01,00,00,00]` = head symbols of the HUFFVAL lists in BITS order (AC-L = `0x01`,
  DC-L = `0x00`, AC-C = `0x00`, DC-C = `0x00`). This fits the standard tables exactly.
  `+0x184` would then be reserved.
- Counter-argument: In the (unused) lossless layout 1 is also at the first position,
  although there DC-L comes first and its head is 0.
- Alternative [?]: The hardware finds the head itself, namely as the symbol with code 0 and
  minimal length (canonical).
- **Irrelevant for the implementation:** The vendor supports only the hard-coded Annex K
  tables. There are no `SetJpegeQl`/custom tables in this libimp
  (grep empty). Taking over the constants 1:1 is enough, and that is exactly what the code does.

**Codebook chain (samples from `.rodata`)** [V]:

| Table | Entry | Value | Meaning |
|---|---|---|---|
| AC luma | [0] | `0x01000002` | symbol `0x01`: code `00`, next `0x02` |
| AC luma | [159] | `0x0FFFFEFF` | symbol `0xFA`: code `0xFFFE`, length 16, next `0xFF` = end |
| AC luma | [160] | `0x0A07F924` | ZRL |
| AC luma | [161] | `0x03000A04` | EOB |
| DC chroma | [11] | `0x0A07FEFF` | end of the list |

**Lossless mode** (bit 9, `0x4b7bc ff.`):

- DC codebooks with **18** entries: 12 copied, 6 zeroed (`0x4b760–0x4b764`). This
  fits the 17 SSSS categories of lossless JPEG.
- Without quant tables.
- ⇒ `LossLess` is real lossless JPEG (process 14) [I strong; the name is now V].

**Alignment and size:**

- `AL_IntermMngr_GetEp1Addr` (L61939, `0x5c9f0`): `addr & 0x1f` ≠ 0 → assert
  (`GetSubBufLocation.part.0`). The 32-byte alignment is hard [V].
- Observed are 256-byte aligned addresses (`0x0718dd00`, `0x0719f600`) [V].
- `0x6400` bytes are allocated, completely zeroed once (`AL_CleanupMemory`, `0x4b224`),
  then the tables up to `0x790` (zeroing the rest `0x738–0x790` is explicit,
  `0x4b7a0`).
- How many bytes the hardware actually reads is open [?]. A user of
  `0x790…0x6400` is not visible. `TraceJpeg` dumps the whole descriptor size as `.tab.hex`
  (`0x4f948`), but that says nothing about the hardware.

**Life cycle** [V]:

- `PushIntermBuffer` copies the master image by `memcpy` into the intermediate buffer
  (`0x6a4c0`).
- Per channel there is exactly **one** EP1 location: `0x8418` is constant per channel in the
  entire trace (table in A5).
- There is no separate cache maintenance for EP1. It is contained in the 1 MiB flush of every job
  (A4).

### A3. JFIF header, restart markers, EOI

Evidence [V] for "no software generates headers":

1. **No marker writer:** In the whole HLIL there is no `0xffd8`, `0xd8ff`,
   `0xffdb`, `0xffc4`, `0xffe0`, `0xffda` or `0xffd9` in a code context and no
   byte stores of `0xd8/0xdb/0xc4/0xe0`. The only hits are `st_size` of
   ELF symbols. There is no string "JFIF".
2. **`generateNals` is empty** (L51582, `0x4b1d8`, `__pure`).
3. **`updateHlsAndWriteSections`** (L51609, `0x4b298`) only copies the existing
   section `{offset, len, ...}` from the part table in the stream buffer and appends an
   end-section entry with flag `0x20000000`.
4. **`EndJpegEncoding`** (L70417, `0x68ca0–0x68cbc`) enters `{StreamBuf+0x14 (=0x200),
   0x8434, −1, −1}`. So the section begins exactly at the hardware start offset and is
   exactly `0x8434` long.

Indications [I, very strong] that the **hardware** writes SOI/APP0/DQT/SOF/DHT/(DRI)/SOS…EOI:

- The hardware receives `X/Y_Density`, `AspectRatioUnit` and `RestartInterval` (names [V]).
  The first three act exclusively on the APP0 header. `RestartInterval` acts on
  DRI and the RSTn markers.
- EP1 contains zigzag quant bytes. Only DQT needs these; the quantiser uses the
  raster reciprocals.
- EP1 contains BITS and a HUFFVAL chain. Only DHT needs these; the entropy coder
  needs only code and length.
- rvd or the IMP API deliver usable JPEG files although the libimp writes no header.

Restart markers: `RestartInterval` is 0 by default (memset in `AL_Settings_SetDefaults`,
no JPEG default assignment in `AL_sSettings_SetDefaultJPEGParam` L43966), in the trace
[31:16] = 0. ⇒ No DRI/RSTn in stock operation [V].

EOI and padding: [?]. Whether `0x8434` still contains filler bytes after `FFD9` (e.g. an
alignment to 4/8/32 bytes) is not evidenced. In the vendor this would be harmless because
decoders ignore bytes after EOI.

Stream content in the trace: **none** [V]. The logger records only WR values. RD values,
RD addresses and stream buffers are not logged; there are only AVC CL dumps.
`TraceJpeg` would write `.bit.hex` with `min(buffer size, status length)` (`0x4f984–0x4f9f4`),
but is never active in IMP.

### A4. Buffer and cache requirements

**Stream buffer** [V trace]:

- Base 256-aligned.
- Word 8 = size (`0xe7680`/`0x27780`, multiples of `0x80`)
- Word 9 = `0x200`
- Word 10 = size − `0x200`
- **New:** The JPEG channel uses the same two-element pool as the AVC channel of the same
  resolution. `CL[048]` = `06e8d700`/`06f75300` (1080p, 16×) and `07135600`/`0715d000`
  (360p, 18×) appear as `0x841C` of the JPEG jobs (T131 vs. T398/T740).
- The buffer size is therefore the AVC max NAL size of the resolution and not a JPEG-specific
  formula. A minimum size is not evidenced [?].
- Word 8 vs. 10 [I]:
  - Allegro convention like AVC `CL[049..051]`: buffer size (end or wrap limit),
    write offset, permitted budget.
  - When the budget is exceeded, `0x8438` bit 1 reports the overflow.
  - The rounding to 32 bytes in the AVC overflow computation (`0x6d9a4`) suggests that
    size and budget should be at least 32-byte granular [I].

**Source buffer:**

- NV12 semi-planar [V]: one srcC pointer (word 5) and ChromaMode 1.
- 4:2:2 and 4:0:0 are possible according to the fields [V], but are not used by IMP.
- Pitch u16 [V].
- UV = Y + pitch·align16(H) observed (1920·1088, 640·368) [V].
- Addresses 256-aligned [V observed]; nothing is checked in software [V].
- Width:
  - `IMP_FrameSource_SetChnAttr` enforces `picWidth` as a multiple of 16 (L96211,
    `0x9ca00`) [V].
  - Allegro checks "Width shall be multiple of 2 on 420".
  - The height may be odd relative to 16 (1080) [V].
  - Which rows the hardware reads for the last MCU row is open [?]. With 4:2:0
    and H = 1080 probably luma up to row 1087 and chroma up to 543. The
    framesource buffers have these rows (1080p = `0x2fd000` = 1920·1088·1.5,
    `src/kernel_interface.c:465`).
- The vendor reads the framesource frame **directly**, the same one AVC is currently
  using: JPEG `0x8410` = AVC `CL[032]` = `0x071a5f00` [V].

**Cache maintenance in the vendor** [V]:

- In `libimp` there are exactly two callers of `Rtos_FlushCacheMemory(ptr, 0x100000)`:
  - `AL_EncCore_Encode1` (`0x6cc84`)
  - `AL_EncCore_EncodeJpeg` (`0x6cdf8`)

  The call goes via `IMP_FlushCache` → `/dev/rmem` ioctl `0xc00c7200` with `dir = 1`
  (L46615, L23910).
- `Rtos_InvalidateCacheMemory` (`dir = 2`, L46610) has **no** caller.
- So the vendor does neither an invalidate of the source frame nor of the stream buffer
  after the completion. It relies on the 1 MiB flush before every job. On MIPS
  (r4k cache code: `_dma_cache_wback` = `r4k_dma_cache_wback_inv`; size ≥
  D-cache/L2 → `blast_dcache`/`blast_scache`) this is a global writeback+invalidate
  of the whole cache [I, kernel sources of rmem not in the repo].
- After that the CPU does not touch the stream buffer until the completion, which is why reading
  after the IRQ is coherent. In the trace there is a gap of ≈120 µs between RMW `0x8014` and `WR 0x85f0` each
  time (e.g. T151 → T154, 128.883279 → 128.883402). This is the
  ioctl call, which does not appear in the avpu log [I].

### A5. Stock sequence line by line, compared with the reconstruction

Once (T69–T88, 128.741):

- `0x8010 = 0x1000`
- `0x83F0` = 1, 2, 4
- `0x8018 = 0xFFFFFF`
- `0x8054 = 0x80`
- WAIT_IRQ thread starts (T86)

JPEG channel creation (T90, T95):

- `0x8014` = `0x10` per JPEG channel, **before every frame**.
- This is `AL_EncCore_SetJpegInterrupt`, called from the channel creation for codec 4
  (L71109, `0x69f44`). **New [V]**: Bit 4 is set already at channel creation, not
  only per job.

Per JPEG job (34 starts, 33 completions visible; the missing one is in a dmesg gap):

1. Only at the first job: RD, `WR 0x85F4 = 1` (T146). The reason is in A1: GC-off applies
   only to cores `< numCore`.
2. RD, `WR 0x8014 = 0x11` or `0x10`, depending on whether AVC is running. 32 of 32 cases
   are fully logged.
3. ≈120 µs cache flush (not logged).
4. `WR 0x85F0 = 1`, `0x8400…0x8428` in ascending order, `WR 0x85E4 = 1`.
5. `WAIT_IRQ` ret, exactly three RD (`0x8430/34/38`). Afterwards no WR, no ack, no
   mask change.

Run times (start → WAIT_IRQ return):

| Resolution | Measurements | Range |
|---|---|---|
| 1080p | n = 15 | 7.87–8.30 ms |
| 360p | n = 17 | 0.92–1.27 ms |

AVC completion:

- RD, `0x8014 = 0x10` (`DisableEnc1Interrupt` clears only bit 0), RD,
  `0x83F4 = 0x00010000` (T252–T261).
- Bit 4 stays set permanently.
- `0x83F4` reads back `0x10001`, so bit 16 is a hardware status bit [I].

There are no other registers: The set of registers written in the entire trace is
{`0x8010`, `0x8014`, `0x8018`, `0x8054`, `0x83E0`, `0x83E4`, `0x83F0`, `0x83F4`,
`0x8400`–`0x8428`, `0x85E4`, `0x85F0`, `0x85F4`} [V].

JPEG jobs never overlap (core 1 serial), but run in parallel to AVC [V].

The reconstruction (`codec-t40.c:7703–7712`) has the same order:

- GC RMW
- mask RMW
- reset
- zone
- start

Deviations: It writes `0x85F4` anew at every job, which is harmless. It clears bit 4
again after every job (`:7730`); the vendor never clears it. Both are permissible,
but cause two more ioctls per job.

---

## Part B – Review of the implementation

### B0. Checked and correct

- **EP1 (`src/hw_encoder.c:912–1004`)** [V]: byte-identical to the vendor for
  q = 1…100 (4:2:0).
  - Method: tables extracted from L131064–131125, `InitQuant`/`InitHuffman` rebuilt in Python,
    compared against the compiled function, 0 deviations.
  - The computed codebooks, including the next chain, also match the
    `.rodata` constants.
  - The probe generator is identical according to the commit. It uses the same
    construction method (`k_natural` = `ZIGZAG_SCAN`).
- **Command values (`codec-t40.c:7685–7695`):**
  - Word 0 `0x131`, word 1 `(W−1)<<16 | (H−1)`, word 2 `0x00010001`, word 3 pitch,
    words 8/9/10 = size/`0x200`/size−`0x200`. All as in stock.
  - Register order as in stock (A5).
- **Clock RMW** `(clk & ~3) | 1` corresponds exactly to `SetClockCommand` (`0x6c4b8`).
- **IRQ demux (`:5121`):** Slot 4 is intercepted before the AVC owner logic.
  - `in_flight` is set before the start (`:7698`), so there is no lost wakeup.
  - The mask is retained on AVC completion (`:4814` writes `0x10` as long as a job
    is running).
  - In serialised operation `0x8014 = 0` after AVC is uncritical, because no JPEG job
    is open.
- **Status evaluation** bit 1 → fallback per frame (`:7740`). This corresponds to the
  vendor overflow.
- **Companion stage** is off with `OPENIMP_T31_HW_JPEG=1` (`:7797`). This is mandatory,
  because otherwise it would kill every JPEG job by `0x85F0`/`0x85E4`.
- **Concurrency:** All encodes run under `g_t31_encode_core_lock` (`:9773`); the
  global single context `g_t31_hwjpeg` is thus protected.
- **Source layout:** pitch = width, UV after align16(H) rows.
  - The P2 JPEG frame is a heap copy with `source->size` bytes
    (`openimp_p2_encoder.c:261–283`).
  - The layout comes from V4L2: `sizeimage` 1080p = `0x2fd000` = 1920·1088·1.5
    (`kernel_interface.c:465`), matching the stock UV offset.
  - Holds [V for stock, I for OpenIMP FS] as long as `bytesperline == width`.
  - The check `width % 16 == 0` (`:7658`) corresponds to the framesource rule
    (`0x9ca00`).
- **NV21 is rejected** (`:7659`). Right, the hardware knows only CbCr order
  (NV12) [I].

### B1. Completion depends on the AVC IRQ thread – start race, JPEG-only, teardown (high)

`t31_hwjpeg_setup` (`codec-t40.c:7612`) opens only the pooled fd. A `WAIT_IRQ` thread
exists on T31 only if an **AVC** context has started it (`:8325–8350`,
`g_tseries_irq_host`), and it ends together with the host context (`:7268–7280`).

- **Start race:**
  - The AVC thread copies the JPEG frame under `p2_core_lock` and then calls
    `AL_Codec_Encode_Process` (`openimp_p2_encoder.c:1233–1239`).
  - The woken JPEG thread can get `g_t31_encode_core_lock` before the very first
    AVC `Process()`. The device is opened lazily there (`:8253`).
  - Consequence: The IRQ lands in the kernel list without a waiter, after 200 ms a timeout occurs,
    and `t31_hwjpeg_disable` switches HW JPEG off **for the whole process lifetime**.
- **JPEG-only:** There is never a waiter and thus never HW JPEG. In addition the
  global init is missing: `0x8010 = 0x1000` and `0x8054 = 0x80` are written only by the AVC session init
  (`:8995–9000`). The vendor always does it (T69–T88).
- **Teardown/re-creation** of the host AVC channel: The waiter is joined, and in this gap
  (or permanently, if only another AVC channel is left) JPEG jobs run into the
  timeout.

Fix:

- Introduce a process-wide waiter held by the device pool, which is started
  as soon as *anyone* (AVC or HW JPEG) opens `/dev/avpu` and is joined only at the last user. The
  T41 model `avpu_t41_irq_host_get/put` (`:5191–5256`) can be taken over.
- At least: In `t31_hwjpeg_encode` start only if `g_tseries_irq_host` is set,
  otherwise do **this frame** in software (not `disable`).
- In `t31_hwjpeg_setup`, catch up on the global init if no AVC session has done it yet.

### B2. Timeout on `CLOCK_REALTIME` (high, easy to fix)

`codec-t40.c:7714` uses `clock_gettime(CLOCK_REALTIME)` with a statically
initialised condvar (default clock REALTIME, `:7555–7558`).

- A forward jump of the clock during the ≈8 ms wait makes `pthread_cond_timedwait`
  return `ETIMEDOUT` immediately. The NTP step at boot happens exactly when streams
  start; the same log says "time disparity of 3298 minutes".
- The consequence is `disable("completion timeout")` for ever.
- In addition the timeout path writes `0x85F0 = 1` into a job that is still running (`:7732`).

Fix: Initialise the condvar with `pthread_condattr_setclock(CLOCK_MONOTONIC)`, for
example via `pthread_once`. Also: After a first timeout, do not deactivate permanently at once,
but e.g. only after three consecutive ones. Read and log `0x85F8` diagnostically before the reset.

### B3. Buffers are sized only once (medium)

`codec-t40.c:7662–7668`: `setup` allocates `src`/`stream` for the **first** resolution.
Both JPEG channels share the global context. With rvd (1080p + 360p) it depends on the
thread order. If 360p comes first, 1080p runs permanently in software, and
`IMP_LOG_INFO` (`:7666`) logs **every frame**.

Fix: Re-allocate the larger size when needed (free the old one with
`avpu_release_dma_buf`) or create separate buffers per resolution or channel. Throttle the log to
once.

### B4. Quality fixed at 75 (medium, functional)

`codec-t40.c:9719` passes a fixed `75u`. In the vendor the quality = `iInitialQP`
(ChParam+0x80 = `tRCParam+0x18`; L46027 `printf("iInitialQP=%d")`, used in `0x4b250`).

The P2 layer already loses values > 51 at the parameter set
(`openimp_p2_encoder.c:751–752`: `qp > 51 → 26`). `IMP_Encoder_SetDefaultParam` limits
to 1…99 (`:1570–1572`).

Fix: For `IMP_ENC_TYPE_JPEG` carry the quality 1…100 unrestricted in a separate field,
e.g. `params+0x84` without the 51 clamp only for JPEG, and read it in `t31_hwjpeg_encode`. Then EP1 is rebuilt automatically on a change (`:7672`).

Note: With one global EP1 and two channels of different quality, EP1
would be rebuilt and flushed at every channel change. That costs little, but one
EP1 per channel would be cleaner, as in the vendor.

### B5. EP1 remainder not zeroed (low to medium)

- `avpu_alloc_encoder(fd, 0x1000, ...)` (`:7619`) returns rmem without zeroing.
- `HW_Encoder_BuildJpegEp1` zeroes only `0x790` bytes (`hw_encoder.c:986`), the flush covers
  only `0x790` (`:7676`).
- The vendor zeroes the whole EP1 (`0x6400`, `AL_CleanupMemory` `0x4b224`). Which
  length the hardware reads is unknown (A2).

Fix: After the allocation, do a single `memset(ep1.map, 0, ep1.size)` and flush the whole
`ep1.size`.

### B6. Source copy without chroma padding rows (low)

`src_size = uv_offset + W·ceil(H/2)` (`:7647`):

- Allocation and copy end after 540 chroma rows.
- If the hardware reads up to chroma row 543 for the last MCU row [?], it reads up to
  4·W bytes behind the buffer. This is only reading, so harmless, but the edge gets
  random data and thus visible artefacts at the lower border.
- The framesource frame has these rows (`0x2fd000`).

Fix: Allocate `src_size = W·align16(H)·3/2` and copy `min(frame->size, src_size)`.

### B7. Cache direction and coherence (medium, verify on the device)

- After the completion, the implementation invalidates with `dir = 2` (`:7752`), before the
  start the whole stream buffer with `dir = 2` (`:7683`).
- The proven T31 AVC path uses `dir = 0` after the completion (BIDIRECTIONAL,
  `:5645`).
- The vendor never uses `dir = 2` (A4).
- Whether the T31 rmem ioctl correctly implements `dir = 2` as `DMA_FROM_DEVICE` is not
  evidenced.

Fix: Use `dir = 0` as in the AVC path. Keep treating the source with `dir = 1`
(on MIPS this is wback+inv anyway).

Note: The code contains contradictory comments on whether the cached
rmem flush is reliable on T31 (`:468–472`, `:757–759`). The AVC stream path therefore uses
an uncached `/dev/mem` alias for the compaction (`:8381–8391`).

- If the probe works with coherent `GET_DMA_MMAP` buffers but the library shows
  artefacts: use `avpu_alloc_mmap` or
  `avpu_remap_uncached` for `ep1` and `stream`.
- The stream is small, so an uncached read is acceptable there.
- `src` should stay cached because of the cost of the 3 MB memcpy.

### B8. EOI check too strict (low to medium)

`codec-t40.c:7754–7760` demands `FFD9` exactly at `length−2`. If the hardware pads
to a word boundary after EOI (A3, [?]), HW JPEG is switched off permanently at the first frame
although the output is valid.

Fix: Search backwards for `FF D9` in the last ≤ 64 bytes, shorten to that length and
switch off only without a hit. Keep checking SOI strictly.

### B9. Mask manipulation by third parties during a job (low)

`AL_Codec_Encode_Destroy` does **not** run under `g_t31_encode_core_lock`:

- An AVC teardown writes `0x8014 = 0` and `0x8018 = 0xFFFFFF` (`:7232–7236`).
- If a JPEG job is running, its IRQ is lost. A timeout follows and thus the
  permanent switch-off (B2).

Fix: Put teardown masking and ack under the same lock as the encode, or in the
teardown write `t31_hwjpeg_idle_irq_mask()` instead of 0 and acknowledge the IRQ pending bits only for
core 0 (`0x0F`).

Also: A stale pending bit 4 from an aborted predecessor process would count immediately as a completion
when unmasking. This is relevant only if JPEG runs before the
AVC init that clears `0x8018`. Hardening: write `0x8018 = 0x10` before unmasking.
According to the kernel, the register is W1C (`avpu_ip.c:121`).

### B10. Smaller points

- `t31_hwjpeg_set_irq_bit(fd, 0)` after every job (`:7730`) deviates from the vendor.
  Harmless. Setting bit 4 once would be closer to the vendor, but would change the
  mask logic in `:4814`.
- The RMW on `0x8014` is not atomic against the AVC callback (`:4814` writes absolutely).
  Because of the serialisation, this can only occur after an AVC "serialized completion timeout"
  (`:9790–9806`). The consequence would be an unnecessary bit 0, harmless.
- Timeout 200 ms with ≈8 ms hardware time: appropriate.
- Cost (design, not a bug):
  - Double 3 MB copy at 1080p (P2 fan-out + `:7679`) plus flush, all under the
    global encode lock. AVC is blocked for ≈8 ms hardware time plus the copy.
  - Irrelevant at 1 fps, noticeable with MJPEG at 10–15 fps.
  - Medium term: copy the fan-out directly into a DMA buffer or, like the vendor,
    reference the FS frame by physical address as long as the frame reference is held.
- `stream_size = W·H/2 + 64 KiB` (`:7648`) is only 16-byte granular for arbitrary
  permitted sizes. Round up to 256 (or 4 KiB), as in stock (A4).
- DMA buffers and the pool reference are never released. As long as the process lives,
  `/dev/avpu` stays bound, even if all AVC channels are destroyed. Within one process
  this is uncritical because the fd is pooled; but it should be documented.
- `OPENIMP_T31_HW_JPEG=1` turns off the companion stage for AVC **at the same time**. Validate
  phase 0 first (companion off, HW JPEG off), otherwise AVC regressions
  cannot be attributed.
- Probe (`tools/t31_hwjpeg_probe.c`): The sequence is correct, and the coherent buffers
  avoid cache questions.
  - Useful addition: `-c <word0>` (test bit 8 and bits 13:12, restart interval in
    [31:16]).
  - Useful addition: `-s <stream-size>` (overflow test).
  - Print `0x85F8` before the start and after the completion.

---

## Part C – Remaining unknowns (only clarifiable on the device)

1. Meaning of word-0 bit 8. Test with the probe: `0x031` instead of `0x131`.
2. Meaning of `0x8430` and of the bits of `0x8438` other than bit 1. `0x85F8` for JPEG.
3. Whether the hardware really writes the complete JFIF stream. This is very
   likely; the probe shows it immediately. In addition: Does the output end exactly with `FFD9`,
   or is there padding?
4. Effect of `RestartInterval` (DRI/RSTn) and `AspectRatioUnit` in the header.
5. How many bytes of EP1 the hardware reads, and whether `+0x180` are head symbols. Test:
   set `+0x180 = 0` and see whether the DHT HUFFVAL order changes.
6. Row access for H mod 16 ≠ 0 (chroma padding) and minimum sizes and alignments
   of stream and source. 256 B are observed; only the EP1 alignment of
   32 B is hard-checked.
7. Whether the `rmem` ioctl `dir = 2` invalidates correctly on T31 (B7).

## Appendix: EP1 verification

The script `re/vendor.py` in the scratchpad parses the hex dump L131060–131135 (addresses
`0xe2fc0–0xe35f0`) and simulates `JpegTables_InitQuant` (L51669) and
`JpegTables_InitHuffman` (L51767, colour path `0x4b67c–0x4b918`). It was compared with
`re/ep1test.c`: These are the lines `hw_encoder.c:662–732` and `:912–1004`, compiled
unchanged.

Result: `mismatching q: 0` for q = 1…100. `ZIGZAG_SCAN` is a permutation
(zigzag→raster) and corresponds to `k_natural` in the probe.
