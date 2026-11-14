# T31 OSD in the vendor libimp: reverse engineering and implementation plan for OpenIMP

> **Status (2026-11-14):** OSD on T31 is now drawn by the IPU backend, on by default, `OPENIMP_T31_OSD=0` turns it off (`src/t31/openimp_t31_services.c:122,146`). The statements below that OpenIMP draws nothing on T31 are the state of 2026-09-29; the rest is historical research.

Status: 2026-09-29. Pure research, no repository was changed.

Markings: **[V]** = verified (source read directly), **[I]** = inferred/plausible,
**[?]** = open, can only be clarified on the device.

Sources:
- `H:` = `libimp.so_hlil.txt` (Binary Ninja HLIL of the T31 libimp, local file, not in the repo; line numbers
  plus address)
- `OI:` = OpenIMP (fork `Lu-Fi/openimp`, branch `claude/t31-hw-jpeg`, commit `01e7eff`)
- `K:` = IPU kernel driver (GPL) from Thingino,
  thingino-firmware (branch `aperto`) `package/all-patches/linux/3.10.14/0001-from-3.10.14-to-thingino.patch`
  (`drivers/video/jz_ipu/jz_ipu_v13.{c,h}`, `jz_regs_v13.h`; in the patch from line 2050755 / 2051895 /
  2052367) and `0007-ipu-wedge-mitigation.patch`
- `VH:` = vendor header T31 1.1.6, ingenic-headers `T31/1.1.6/en/imp/` (timps submodule `include/`)
- `TP:` = timps sources timps (`Lu-Fi/timps`) `src/hal/`

---

## 0. Summary

- **The OSD is mixed.** `OSD_REG_PIC`, `OSD_REG_PIC_RMEM` and `OSD_REG_COVER` are blended by the
  **hardware IPU block** (`/dev/ipu`, kernel driver `jz_ipu_v13`) directly into the NV12 frame
  (in place). `OSD_REG_LINE`, `OSD_REG_RECT` and `OSD_REG_BITMAP` are drawn opaquely by the **CPU** into
  the NV12 frame. **[V]**
- Neither the ISP (tx-isp) nor the AVPU encoder play a role here. `open-tx-isp` has no
  OSD interface for T31. **[V]**
- The call per frame: A thread named `group_update` delivers the frame to the
  OSD group. There `osd_update` → `osd_process` → `OSD_Draw_Layer_Cover_Pic` →
  `ipu_osd` → `ioctl(/dev/ipu, IOCTL_IPU_START)` run. Then the frame goes to the encoder. **[V]**
- **Why the measurement shows ~1 % per region and stream, almost independent of the area:** For normal PIC and COVER regions, the libimp
  does **one IPU run per region and per frame** (batch size 1). Each run copies the bitmap again into an IPU buffer and does a
  cache flush over a **fixed 1 MiB** (`IOCTL_IPU_BUF_FLUSH_CACHE`). Then come the ioctl, the IRQ and the
  wait. These fixed costs occur for static text just as for changing text. **[V]** for
  the mechanism, **[I]** for the cost distribution.
- **OpenIMP:** The OSD group is not in the frame path at all today. But there is an ideal hook
  (`OI: src/t40/openimp_p2_encoder.c:1213–1239`, between `IMP_FrameSource_GetFrame` and
  `AL_Codec_Encode_Process`). In addition, the OSD structures in OpenIMP are **ABI-incompatible**
  with the vendor headers that the streamers are built with. `IMP_OSD_GetRgnAttr`
  overwrites the caller's stack. **[V]**
- **Feasibility: good.** Recommended is an IPU backend modelled on the libimp, but
  more efficient: bitmaps stay permanently in DMA memory, up to 4 layers per IPU run, no
  flush per frame. In addition a small CPU backend for LINE/RECT/BITMAP (and as a fallback).
  **Effort about 7–10 person-days** including device tests.

---

## 1. Where is OSD applied? (question 1)

### 1.1 Finding per region type

| Region type (`VH: imp_osd.h`) | Where | Evidence |
|---|---|---|
| `OSD_REG_PIC` (5) | IPU hardware, bitmap is copied into the IPU buffer per frame | `H:111932–111978` (layer setup), `H:110515/110560/110615/110665` (memcpy in `ipu_osd`) **[V]** |
| `OSD_REG_PIC_RMEM` (6) | IPU hardware, bitmap is in the OSD pool (rmem), physical address is passed directly, up to 4 layers per run | `H:111978` (`osd_mem_vir_to_phy`), `H:111549f` (`var_4c = 4` for type 6) **[V]** |
| `OSD_REG_COVER` (4) | IPU hardware in **mask mode** (single colour, no memory access) | `H:111704–111777`, `H:110487` (`_ipu_set_osdx_mask` with data pointer 0) **[V]** |
| `OSD_REG_RECT` (2) | CPU: 4 × `osd_draw_line` | `H:112464–112520` **[V]** |
| `OSD_REG_LINE` (1) | CPU: `osd_draw_line` | `H:112747` **[V]** |
| `OSD_REG_BITMAP` (3) | CPU: loop over 8-bit mask | `H:112554–112745` **[V]** |

### 1.2 Hardware block: IPU (`/dev/ipu`)

- `ipu_init` opens `open("/dev/ipu", O_RDWR)` (`H:110326`). The error message is
  `"ipu: Cann't open /dev/ipu"`. **[V]**
- ioctl numbers in `ipu_osd`: **[V]**
  - `0x20004976` (`H:110684`) = `_IO('I', 118)` = `IOCTL_IPU_BUF_FLUSH_CACHE`. Argument
    `{void *addr = ipu_vbuf, unsigned size = 0x100000}`. The kernel calls
    `dma_cache_sync(addr, size, DMA_BIDIRECTIONAL)` with it (K: `ipu_ioctl`).
  - `0x2000496a` (`H:110690`) = `_IO('I', 106)` = `IOCTL_IPU_START`. Argument: a 0x98 byte
    `struct ipu_param` (see §3.1). The kernel copies it, programs the registers,
    starts the IPU and waits synchronously for the OUT_END IRQ (K: `ipu_start`).
  - The MIPS encoding of `_IO` has bit 29 set, which gives `0x2000_49xx`.
- Kernel configuration: T31 kernels 3.10.14 in Thingino have `CONFIG_JZ_IPU=y`,
  `CONFIG_JZ_IPU_V13=y`
  (`thingino-firmware/board/ingenic/xburst1/kernel/3.10.14/t31.generic.config:1185–1187`). **[V]**
  The driver registers the misc device `ipu` (K: `ipu_probe`, `sprintf(ipu->name,"ipu")`). **[V]**
- Register block (K: `jz_regs_v13.h`): OSD channels 0–3 have `IPU_OSD_IN_CHx_{Y,UV}_{ADDR,STRIDE}`
  (0xbc–0xf8), `IPU_OSD_CHx_GS` (0xfc–0x108), `IPU_OSD_CHx_POS` (0x10c–0x118),
  `IPU_OSD_CHx_PARA` (0x11c–0x128), `IPU_OSD_CH_BK_PARA` (0x12c) and `IPU_OSD_CHx_BAK_ARGB`
  (0x130–0x13c). Only the kernel programs these registers; userspace sees only the
  ioctl ABI. **[V]**
- The background is the NV12 frame itself. Input and output are the **same physical
  address**: Y at `bg_buf_p`, UV at `bg_buf_p + bg_w*bg_h`, stride = `bg_w`
  (K: `_ipu_set_bg_buffer`, `_ipu_set_bg_route`). So the OSD is in place. **[V]**
- Userspace does **not** write any frame data by CPU here. The frame is passed only via its
  physical address (`frame+0x18`) (`H:111704–111708`, `var_1c8 = *(frame+0x18)`). **[V]**

### 1.3 No ISP or encoder involvement

- In the T31 HLIL there are no `*_ISP` OSD functions (grep for `osd.*isp`/`IMPIspOsd` is
  empty). **[V]**
- `open-tx-isp/driver/t31` has only an unused field `ipu_clk`
  (`driver/t31/include/tx_isp_core_device.h:31`) and no OSD ioctls. **[V]**
- The AVPU encoder receives the already overlaid frame. **[V]** (order see §2)

### 1.4 CPU drawing path (LINE/RECT/BITMAP)

- It writes directly via the virtual frame address (`frame+0x1c`). The stride is
  `align16(width)`, the UV plane starts at `align16(height) * stride` (`H:111105–111182`,
  `H:112736f`). **[V]**
- It is opaque, without alpha blending (details §3.6).
- The CPU draws **before** the IPU pass: `osd_process` calls `OSD_Draw_Layer_Cover_Pic` only after
  the loop (`H:112756`). **[V]**
- Whether the vendor does cache maintenance in between is open **[?]**. No flush is visible in
  `osd_process`. Probably the frame mapping of the vendor VBM is uncached, or the
  effect was never noticed.

---

## 2. Data flow per frame (question 2)

### 2.1 Graph and thread

1. `IMP_OSD_CreateGroup(n)` (`H:112813`):
   - allows only `n < 4` (`H:112815`) **[V]**
   - calls `create_group(4 /*DEV_ID_OSD*/, n, "<name>-n", osd_update)` (`H:112837`) **[V]**
   - creates 512 node slots for the group (`H:112840ff`) **[V]**
2. `IMP_System_Bind(FS→OSD)` and `(OSD→ENC)` attach the groups as observers. The
   generic `group_update` (`H:21988–22005`):
   - sets the thread name via `prctl(PR_SET_NAME, "group_update")`
   - calls the update callback of the group (for OSD `osd_update`)
   - returns the frame via `VBMReleaseFrame` on error or when there are no observers **[V]**

   This is where the "group_update" thread pair from the measurement comes from: one thread per stream that hands the
   frames from the FrameSource to the respective OSD group. **[I]**
3. `osd_update(module, frame)` (`H:112761–112791`):
   - calls `osd_process(module, frame, 0)` **synchronously** **[V]**
   - then passes the frame on to the observers (encoder group), with several observers
     using `VBMLockFrame` **[V]**
   - So the OSD is finished before the encoder sees the frame.
4. Special path `osd_update_left` (`H:112793–112811`):
   - Frames with flag byte `frame+0x3e4 == 1` are "low-latency/partial frames". Here
     the libimp reads the line counter of the ISP via `read_reg_32_addr(*(frame+0x3e8))` and waits until
     the region lines are written ("OSD wait too long yAvail…", `H:112106–112160`). **[V]**
   - OpenIMP always delivers finished frames, so this path is **not needed**. **[I]**

### 2.2 `osd_process` (`H:112373–112759`)

- It takes the semaphore `semRgnList` (device base +0x2b0a0, `H:112378`). Every API function that
  changes region data takes this semaphore: `IMP_OSD_UpdateRgnAttrData` (`H:113457`),
  `IMP_OSD_SetRgnAttr` (`H:113797ff`), `IMP_OSD_ShowRgn` (`H:114867`) and `IMP_OSD_CreateRgn`.
  Per-frame processing and all API updates are thus mutually exclusive. An
  API call blocks for at most the duration of one group pass (including IPU run time). **[V]**
- It processes the group only if it is "started". `IMP_OSD_Start`/`Stop` only set
  this flag (`H:114896`). **[V]**
- It runs over the list of nodes of the group. The list is **sorted by `layer` in
  ascending order**:
  - `IMP_OSD_RegisterRgn` appends a node at the end (`H:113282ff`).
  - `IMP_OSD_SetGrpRgnAttr` re-sorts it by `layer` (`H:114648ff`).
  - Later nodes are on top. **[V]** for the sorting, **[I]** for "ascending = on top".
- It skips regions with `show == 0` (`H:112462`). **[V]**
- For every visible region of type LINE, RECT or BITMAP it draws immediately by CPU.
  At the end it calls `OSD_Draw_Layer_Cover_Pic(frame, list, osd, 0)` for COVER, PIC and PIC_RMEM
  (`H:112756`). **[V]**
- With `SetRgnAttrWithTimestamp` a timestamp FIFO hangs on the region. The region applies only to
  frames whose timestamp (`frame+0x20/0x24`) is in the window `[ts-minus, ts+plus]`
  (`H:112386–112455`). **[V]** For the first version this is optional.

### 2.3 `OSD_Draw_Layer_Cover_Pic` (`H:111495–112371`)

- It builds a 0xa8 byte userspace structure with a header and 4 layer slots of 0x24
  bytes each. The fields per layer: fmt, para, x, y, w, h, vaddr, paddr, color. **[V]**
- It sets the enable bit `1<<slot` (`H:111704`). **[V]**
- Header: **[V]**
  - `bg_w` = `align16(frame.width)`
  - `bg_h` = `align16(frame.height)`
  - With `frame+0x28 == 1`, height and width are swapped, presumably for rotated frames
    (`H:111558–111566`). **[I]** for the meaning.
  - `bg_fmt` = `0x18` (HAL NV12)
  - `bg_buf_p` = `frame+0x18`
- **Batch size** `var_4c`:
  - It is 1, except in low-latency mode or when a region of type `PIC_RMEM` occurs.
    Then it is 4 (`H:111503–111506`, `H:111549f`). **[V]**
  - After each layer it calls `ipu_osd()` as soon as `var_50 >= var_4c`, and resets the structure
    (`H:112067–112100`). **[V]**
  - For normal PIC and COVER regions this means **one IPU run per region**.
- Check per region:
  - It only checks whether the **smallest** coordinate is inside the picture: `min(p0.x,p1.x) <= w-1`
    and `min(p0.y,p1.y) <= h-1`. Otherwise the message "invalid param … keep the smallest
    coordinates within the picture range" appears (`H:111588`). **[V]**
  - An overhang to the right or bottom is **not** clipped but passed on to the IPU. **[V]**
  - The error shows only in the IPU, and in every frame (cf. `TP: imp_osd.c:222–233`).
    **[I]**
  - `offPos` from `IMPOSDGrpRgnAttr` goes only into this check. The layer position is
    `min(p0,p1)` **without** `offPos` (`H:111975ff`, `H:111770ff`). **[I]**
  - In the CPU path (LINE, RECT, BITMAP) `offPos` is added. **[V]**
- Debug detail: If `/tmp/mountdir/osdsnap<handle>.pic` exists, the libimp writes the bitmap of the
  region into it (`H:111983ff`). **[V]**

### 2.4 `ipu_osd` (`H:110415–110699`): buffer and locking

- It takes the process-wide `ipu_mutex`. `ipu_buf_lock`/`unlock` are empty (`H:110387–110395`).
  **[V]**
- The IPU scratch buffer is the OSD pool:
  - It is created in `OSDInit` (`H:111395`) with `IMP_Alloc(alloc_ipu, pool_size, "osdDev")`,
    i.e. physically contiguous (rmem).
  - The size is set by `IMP_OSD_SetPoolSize`. The default is tiny, which is why timps sets it
    explicitly (`TP: hal_ingenic.c:1163–1176`).
  - `ipu_init` takes over virtual address, physical address and size (`H:110330–110337`).
    **[V]**
- For every layer with `vaddr != 0 && paddr == 0` (normal PIC): `memcpy` of the whole bitmap into the
  pool, 4-byte aligned one after another, and passing the physical pool address
  (`H:110515`, `H:110560`, `H:110615`, `H:110665`). **[V]**
  - If the pool is not enough: `"ipu buffer too small, OSD need Buffer size is %d"`
    (`H:110461`). **[V]**
  - Identical layers in a batch share the copy (`chx_share_osd_mem`,
    `H:110397–110413`). **[V]**
- Layers with `vaddr == 0` (COVER) go through `_ipu_set_osdx_mask` and the colour in the field
  `bak_argb` (`H:110487`). **[V]**
- Then always follows `IOCTL_IPU_BUF_FLUSH_CACHE` over `{ipu_vbuf, 0x100000}` and then
  `IOCTL_IPU_START` (`H:110681–110690`). **[V]**

### 2.5 How attributes are applied, and `UpdateRgnAttrData`

- `IMP_OSD_CreateRgn` and `IMP_OSD_SetRgnAttr` **copy the bitmap into a separate buffer of the
  libimp**:
  - `calloc(w*h*bpp)` for PIC and BITMAP (`H:113196`)
  - `OSD_mem_alloc` from the OSD pool for PIC_RMEM (`H:113184`)
  - copy by `memcpy` (`H:113052`)

  The caller may free its buffer afterwards. **[V]**
  - bpp on T31: 4 for 32-bit formats, 2 for `PIX_FMT_RGB555LE`/`BGR555LE`, 1.5 for NV12/NV21
    (`H:113131`, `H:113180`).
  - Conversion `Convert1555To8888` (`H:110852`) exists only on T10/T20
    (`get_cpu_id() < 3`); on T31 the data is copied raw. **[V]**
- `IMP_OSD_UpdateRgnAttrData` (`H:113436–113751`) copies the new data **in place** into the
  existing region buffer (size from the stored rectangle). It holds the
  semaphore `semRgnList` (`H:113457–113470`). For LINE, RECT and COVER it updates only
  `color`/`linewidth`. **[V]**
  - There is **no double buffering**. Tearing is prevented by the shared semaphore,
    and the IPU works on a copy per frame anyway. **[V]**/**[I]**
- `IMP_OSD_ShowRgn` sets `show` in the group node. When hiding, it empties the
  timestamp FIFO (`H:114867`). **[V]**
- The parameters of the group node from `IMPOSDGrpRgnAttr`: `show`, `offPos`, `scalex/y`,
  `gAlphaEn`, `fgAlhpa`, `bgAlhpa`, `layer`. In the node they are words [2..10]. Used
  are `show`, `offPos` (CPU path and check only), `gAlphaEn`, `fgAlhpa` and `layer`.
  `scalex`, `scaley` and `bgAlhpa` are **not** read in the drawing path. **[I]** (not referenced
  in the functions read)
- Limits: 4 groups (`H:112815`), 512 regions (`H:111453`, `H:112840`). **[V]**

---

## 3. Region types, formats, mathematics (question 3)

### 3.1 ioctl ABI `IOCTL_IPU_START` (interface facts from K: `jz_ipu_v13.h`)

`struct ipu_param` is 38 × u32 = 0x98 bytes. The libimp builds exactly this size
(`H:110426`, `memset(&str,0,0x98)`). **[V]**

| Word index | Field | libimp assignment for OSD |
|---|---|---|
| 0 | `cmd` | bit 0–3 = OSD channel 0–3 active (`IPU_CMD_OSD0..3`), bit 4 = CSC (not OSD) |
| 1, 2 | `bg_w`, `bg_h` | `align16(w)`, `align16(h)` = stride and Y height of the NV12 frame |
| 3 | `bg_fmt` | `0x18` (HAL NV12) |
| 4 | `bg_buf_p` | physical address of the frame (in place) |
| 5 | `out_fmt` | likewise `0x18` |
| 6 + 8k … 13 + 8k | channel k: `fmt`, `para`, `bak_argb`, `pos_x`, `pos_y`, `src_w`, `src_h`, `buf_p` | see below |

- The kernel computes `src_w*4` (RGB) or `src_w*2` (5551) as the line stride of the bitmap. So the
  bitmap must be present **without padding**. The UV address of an NV12 layer is
  `buf_p + w*h` (K: `_ipu_set_osd_chx_route`, `_ipu_set_osdx_buffer`). **[V]**
- If `para == 0`, the kernel sets a default value. **[V]**
- The kernel does **not** clip and does not check bounds. **[V]**

### 3.2 The `para` word per channel (T31 path)

The bit assignment follows `CH_BK_PARA` in K: `jz_regs_v13.h`, analogously for the channels. **[I]**

- **Bit 0:** channel on.
- **Bit 1–2:** alpha mode. 0 = pixel alpha, 1 = global alpha, 2 = pixel alpha × global alpha.
- **Bit 3–10:** global alpha.
- **Bit 11–13:** picture type. 0 = ARGB family, 1 = 16-bit 5551, 2 = NV12, 3 = NV21.
- **Bit 14–17:** byte order. 0 = ARGB, 5 = ABGR, 8 = RGBA, 0xD = BGRA.
- **Bit 23:** mask mode (colour from `bak_argb`). **[I]**
- **Bit 24–25:** 2 for RGB sources, 1 for NV12/NV21 sources. Meaning **[?]**, copy 1:1.

T31 is recognised like this: `get_cpu_id()` returns the values 15–23 for SoC ID 0x31 (`H:26100–26140`).
The T31 branches are `label_bd830`, `label_bdad8`, `label_c0f20` and `label_c1138`. **[V]**

**Bitmap layer** (`_ipu_set_osdx_para`, `H:110159–110315`; T31 branch `H:110205–110222`,
result `H:110313`) **[V]**:
- The caller passes in `para` a value `alpha | 0x100` if `gAlphaEn <= 0`, otherwise
  `fgAlhpa` (`H:111934`).
- With `0x100` (pixel alpha): base value `0x020347F9`.
- Without `0x100` (global alpha): `0x020347FB` for NV12/NV21 layers (mode 1), otherwise
  `0x020347FD` (mode 2 = pixel × global).
- Then bits 11–17 and 24–25 are replaced depending on the format and
  `para = (alpha & 0xff) << 3 | (v & 0xfffff807)` is set.

**Mask layer / COVER** (`_ipu_set_osdx_mask`, `H:110097–110157`; T31: `H:110145–110155`) **[V]**:
- `((alpha<<3) | 0x02034005) & 0xfffc3fff | 0x00800000` with `gAlphaEn > 0`
  (mode 2 = pixel × global).
- `((alpha<<3) | 0x02034001) & 0xfffc3fff | 0x00800000` otherwise (mode 0 = pixel alpha).

### 3.3 Pixel formats for PIC (table `CSWTCH.41`, `H:137486`, use `H:111928`)

IMP enum according to `VH: imp_common.h:93–139`, consecutive without explicit values **[V]**:

| IMP `PIX_FMT_*` (value) | HAL/IPU code | Meaning (K) | Bitmap bytes per pixel |
|---|---|---|---|
| `ARGB` (14) | 9 | ARGB_8888 | 4 |
| `RGBA` (15) | 1 | RGBA_8888 | 4 |
| `ABGR` (16) | 8 | ABGR_8888 | 4 |
| **`BGRA` (17)** | **5** | **BGRA_8888, in memory B,G,R,A = little-endian `0xAARRGGBB`** | 4 |
| `RGB555LE` (21) | 6 | RGBA_5551 | 2 |
| `BGR555LE` (25) | 0x1a | BGRA_5551 | 2 |
| `NV12` (10) / `NV21` (11) | 0x18 / 0x19 | YUV layer | 1.5 |
| `YUV420P` (0) | 0x13 | rejected by `_ipu_set_osdx_para` ("ipu: err osd fmt not support") | – |
| all others < 26 and ≥ 26 | 5 (like BGRA) | – | 4 |

The column "bytes per pixel" is **[V]**. The breakdown of the ARGB order follows
K: `jz_regs_v13.h` `CH_BK_ARGB_TYPE_*`. **[V]**

For COVER, `fmt == PIX_FMT_BGRA (17)` **must** hold, otherwise "COVER cannot support this
format" appears (`H:111762`). **[V]**

In practice: prudynt and timps use `OSD_REG_PIC` with `PIX_FMT_BGRA` and `OSD_REG_COVER` with
`PIX_FMT_BGRA` (`TP: imp_osd.c:348–353, 432–438`). **[V]**

### 3.4 Alpha logic

- **PIC:**
  - With `gAlphaEn == 0` only the per-pixel alpha from the bitmap counts. `fgAlhpa` is in the
    field, but is ignored by the HW in mode 0 **[I]**.
  - With `gAlphaEn > 0`, pixel alpha × `fgAlhpa` applies (RGB) or only `fgAlhpa` (NV12 layer).
    **[V]** for the bits, **[I]** for the HW semantics.
- **COVER:** The colour `0xAARRGGBB` is converted to "AYUV" in the libimp (§3.5).
  - With `gAlphaEn > 0`, first `A' = min(255, ((fgAlhpa+1) * A) >> 8)` is computed
    (`H:111737`), then mode 2 with global = `fgAlhpa`. So effectively `fgAlhpa` acts twice.
    **[V]**/**[I]**
  - With `gAlphaEn == 0`, `A` stays unchanged (mode 0).
- The exact HW blending and rounding (e.g. `(a*fg + (255-a)*bg + 127)/255` or `>>8`, and whether the
  `CH_BK_PREM` bit that the kernel sets for OSD has an effect) are **[?]**. This can only be
  determined by a device test (§5.6).

### 3.5 Colour conversion in the libimp (COVER colour, LINE/RECT colour)

From `H:111715–111730` (cover) and `H:111100–111110` (line) **[V]**. With R, G, B from
`0xAARRGGBB` the following holds in integers, `>>` arithmetic:

```
Y = (66*R + 129*G + 25*B + 128) >> 8            // no +16!
U = ((-38*R - 74*G + 112*B + 128) >> 8) + 128
V = ((112*R - 94*G - 18*B + 128) >> 8) + 128
Word = A<<24 | Y<<16 | U<<8 | V
```

- Y lacks the usual BT.601 offset +16. White gives Y=219, black Y=0. This is how it is in the
  code, **[V]**. Whether the IPU adds a luma offset for masks (register
  `CSC_OFSET_PARA` = 0x10/0x80) is **[?]**.
- For BGRA bitmaps the **IPU itself** converts RGB to YUV. The coefficients are set by the kernel
  (K: `_ipu_set_osd_chx_route`, about ×1024):
  - Y ≈ 0.299 R + 0.587 G + 0.114 B
  - U ≈ −0.147 R − 0.289 G + 0.436 B
  - V ≈ 0.615 R − 0.515 G − 0.100 B
  - offsets 16 and 128

  This is a mixed form (analogue YUV with luma offset). **[V]** for the constants,
  **[?]** for the exact effect. This is relevant only if a CPU fallback is to have the same
  colours.

### 3.6 CPU paths (LINE, RECT, BITMAP)

**LINE/RECT** (`osd_draw_line`, `H:110885–111389`):
- Drawing happens only if `fmt == PIX_FMT_MONOWHITE (8)` (`H:111098`). The vendor sample
  uses exactly that. **[V]** for the condition, **[I]** for nothing being drawn otherwise.
- Colour per §3.5, written **opaquely**:
  - Y byte per pixel
  - UV pair at `(x & ~1)` in chroma row `y/2` (`H:111182ff`) **[V]**
- Line width `linewidth`, centred (start `pos - lw/2`).
- Nearly horizontal or vertical lines (`|dx| < 2*lw`) are filled as a rectangle, otherwise
  as DDA with float slope. **[V]**/**[I]**
- RECT = 4 lines (`H:112502–112530`).
- Coordinates + `offPos`, clipped to `[0, w-1] × [0, h-1]`.

> **Deviation in OpenIMP** (branch `claude/osd-thick-lines`): The
> stock stamp chain across the line is **not** `lw` wide, but
> `lw*cos(θ) + (lw-1)*sin(θ)` — at 45° exactly `2*lw - 1` rows = `1.41*lw` (at `lw = 8`
> measured 10.6 px instead of 8). OpenIMP therefore draws diagonals **from `lw >= 2`** as a band of
> width `lw` (`round(lw*hypot/major)` pixels per step, integer arithmetic only). Unchanged
> or byte-identical to stock remain `lw = 1` (any direction), exactly horizontal and vertical
> lines and thus RECT. The change is deliberate and visible: Diagonals are drawn **thinner** than in
> stock. Details, measured values and the still open device test:
> `docs/archive/HANDOVER_OIMP_OSDTHICK.md`.

**BITMAP** (`H:112554–112745`):
- 1 byte per pixel (size `w*h`, `H:113470`).
- For every byte `!= 0`, `Y := byte` is written and `0x80` at the chroma position `(x, y/2)`.
  This gives a grey or white mask without alpha. **[V]**

### 3.7 Limits and pitfalls (device, evidenced from timps)

- In rotated mode (`frame+0x28`) the IPU pass fails with odd widths or heights, and
  a faulty layer "poisons" the whole group pass (`TP: imp_osd.c:202–207`).
  **[V as a device observation by timps]**
- Regions outside the picture produce the IPU error in every frame
  (`TP: imp_osd.c:222–233, 327–331`).
- IPU "wedge" about every 500k operations (T20 family). Thingino patches the driver with a 100 ms
  timeout, reset and retry (`0007-ipu-wedge-mitigation.patch`), same driver for T31. **[V]**

### 3.8 Interpretation of the measurement (~0.7–1.1 % per region and stream)

Per normal PIC or COVER region and frame the following occurs (§2.3, §2.4): a `memcpy` of the bitmap,
a `dma_cache_sync` over **1 MiB** (on MIPS r4k at size ≥ cache size practically a
blast of the whole D/L2 cache plus cold caches afterwards), an ioctl, programming of the
registers, IRQ and wakeup. These are fixed costs, independent of area and text change.

At 25 fps, 1 % of a 1 GHz core corresponds to about 400k cycles per region and frame. That fits
a blast of the whole cache and the refills. **[I]**

The `memcpy` grows with the area, but is small at typical 20–50 KB. **[I]**

---

## 4. What OpenIMP has today (question 4)

1. **Only bookkeeping, nothing is drawn** (`OI: src/t31/openimp_t31_services.c:72–319`). **[V]**
   - `IMP_OSD_Start`/`Stop` only set `started` (`:293–319`).
   - `CreateGroup` creates no module and no group in the graph (`:82–90`).
   - `IMP_OSD_AttachToGroup` is missing.
2. **Bind:** `IMP_System_Bind` only remembers edges (`OI: src/t40/openimp_p2_encoder.c:821–850`,
   the T31 build uses this file, `build-t31.sh:48`). `p2_find_source_channel` (`:567–590`)
   runs from the encoder group back to the FrameSource over the bind edges. A
   `DEV_ID_OSD` cell in between is **transparently skipped**, so the OSD group is not
   in the data path. **[V]**
3. **Hook exists:** The encoder pulls frames itself, synchronously in the thread of the caller of
   `IMP_Encoder_PollingStream`. **[V]**
   - `while (IMP_FrameSource_GetFrame(ch->source_channel, &frame) …)` (`:1213`)
   - then `p2_copy_requested_jpeg_frames(...)` (`:1236`, JPEG copy by CPU)
   - then `AL_Codec_Encode_Process(ch->codec, frame, frame)` (`:1239`)

   On T31 the frame is released only after the encoder completion (`:56–65`). A call
   `openimp_osd_apply(osd_grp, frame)` directly after `:1218` and **before** `:1236` gives the
   vendor's semantics: encoder and JPEG snapshot see the OSD.
4. **Frame description fits** (`OI: src/kernel_interface.c:922–941`,
   `openimp_p2_encoder.c:~88–102`, `P2SyntheticFrame`): +8 width, +0xc height, +0x18 phys,
   +0x1c virt, as in the vendor. **[V]**
   - NV12 layout: UV at `width * align16(height)`, `picWidth` must be 16-aligned
     (`framesource_tseries.c:404–418`, `:1203`). This is exactly what the IPU expects with
     `bg_w = align16(w)`, `bg_h = align16(h)`. **[V]**
5. **Memory and cache:**
   - Frames are in the **cached** arena area mapped via `/dev/rmem`
     (`dma_alloc.c:426–447`). **[V]**
   - `DMA_RmemFlushCache(virt, size, dir)` already exists (`dma_alloc.c:928–1044`,
     1 = WBACK, 2 = INV, 0 = both via `dma_cache_sync`).
   - `DMA_AllocDescriptor`, `DMA_VirtToPhys` and `IMP_Free` are available for physically contiguous
     bitmap buffers (`dma_alloc.c:710, 781, 819`). **[V]**
   - The JPEG path invalidates the frame before its CPU copy (`openimp_p2_encoder.c:248`).
     So it would also be correct after an IPU pass. **[V]**
   - There is no IPU use in OpenIMP.
6. **ABI error (critical, independent of drawing)** **[V]**. The streamers are built against the
   vendor headers (timps: `include/T31/...`, `TP: imp_osd.c:17`), OpenIMP against
   its own:
   - `IMPOSDRgnAttr` is **72 bytes** in OpenIMP (test with `gcc -m32`: attr=72, data=48),
     **32 bytes** in the vendor (`VH: imp_osd.h`, `type, IMPRect{p0,p1}, fmt,
     union{8 bytes}`).
   - OpenIMP wrongly nests a second `type/rect/fmt` in `IMPOSDRgnAttrData`
     (`OI: include/imp/imp_osd.h:59–91`).
   - `IMPRect` in OpenIMP is `{x,y,width,height}` (`include/imp/imp_common.h:158–163`), in the
     vendor `{IMPPoint p0, p1}`, i.e. inclusive corner points.
   - `PIX_FMT_BGRA` is **12** in OpenIMP (`imp_common.h:63`), **17** in the vendor.
   - Consequences:
     - `IMP_OSD_GetRgnAttr` (`services.c:226`) writes 72 bytes into a 32-byte structure of the
       caller, the **stack is overwritten**.
     - `CreateRgn`/`SetRgnAttr` (`:131`, `:205`) read 40 bytes beyond the end.
     - `UpdateRgnAttrData` (`:273`) reads 48 instead of 8 bytes.
   - `IMPOSDGrpRgnAttr` is 36 bytes in both and compatible.

   An implementation must use the vendor layouts internally, with `_Static_assert` on
   32 or 36 bytes, and the numeric values of the vendor enum (8, 10, 11, 14–17, 21, 25).
   One should not change the own `IMPRect` in `imp_common.h` globally, because IVS and others
   use it.

---

## 5. Feasibility and implementation plan (question 5)

### 5.1 Architecture decision

An **IPU backend as the main path** plus a small **CPU backend** is recommended.

| Criterion | IPU (`/dev/ipu`) | CPU blend in the frame |
|---|---|---|
| Fidelity to the vendor | identical, HW blend and colour space | approximation, rounding and colour must be calibrated |
| CPU per frame | ~1 ioctl per ≤ 4 layers, **no** flush per frame needed (see below) | ∝ visible pixels; text 400×48 ≈ 0.1–0.5 % at 25 fps; large semi-transparent covers expensive (¼ 1080p ≈ 5–10 %) |
| Cache maintenance | none for the frame (the IPU works physically, the CPU does not touch the frame), flush bitmaps once on update | invalidate row bands and write back after writing (the frame is cached) |
| Risk | IPU wedge (kernel patch exists), driver must exist | no HW dependency |

The cost figures for the CPU column are **[I]**.

- The CPU backend is needed anyway for LINE, RECT and BITMAP, as in the vendor.
- It serves as a fallback when `/dev/ipu` is missing or after repeated IPU errors.
- It serves as a reference for tests.
- Alternative: RECT and axis-parallel LINE can be mapped as **IPU mask layers**
  (4 thin cover layers = 1 IPU run). Then only the slanted line stays on the CPU.

### 5.2 Steps

1. **Repair the ABI** (prerequisite):
   - private structs in the vendor layout in `openimp_t31_services.c`, or a separate header
     `src/t31/osd_abi.h`: `vimp_rect {p0x,p0y,p1x,p1y}`, `vimp_osd_rgn_attr` (32 B),
     `vimp_osd_attr_data` (8 B union)
   - the public prototypes in the `.c` with `void *` or correctly typed
   - `_Static_assert`s
   - constants `VPIX_MONOWHITE=8, NV12=10, NV21=11, ARGB=14, RGBA=15, ABGR=16, BGRA=17,
     RGB555LE=21, BGR555LE=25`
   - region handles `0..511`, groups `0..3` (limits as in the vendor)
2. **Region memory:**
   - On creating or changing, **copy the bitmap into a DMA buffer**:
     `DMA_AllocDescriptor`, size `w*h*bpp`, rows without padding.
   - Then `DMA_RmemFlushCache(buf, size, 1 /*WBACK*/)`.
   - **Double buffering:** Updates write into the inactive buffer, flush and swap the
     pointer under the OSD lock. This way nothing is copied per frame and nothing tears.
   - `IMP_OSD_SetPoolSize` is accepted as a budget or hint.
   - COVER: precompute the AYUV word per §3.5, `fgAlhpa` scaling as in the vendor.
3. **Pipeline hook:**
   - In `p2_find_source_channel`, remember the first `DEV_ID_OSD` cell on the way
     (`ch->osd_group`).
   - After `IMP_FrameSource_GetFrame` (`openimp_p2_encoder.c:1218`) call:
     `openimp_t31_osd_apply(ch->osd_group, frame)`.
   - Only if the group is started and has at least one visible region.
   - Compile only on T31 (`#if defined(PLATFORM_T31)`).
   - Optionally `IMP_OSD_AttachToGroup` as an alias for `IMP_System_Bind`.
4. **IPU backend:**
   - `open("/dev/ipu", O_RDWR)` lazily, once per process, with a mutex.
   - Per frame, sort the visible PIC and COVER regions by `layer` and pack them in groups of
     **4 layers** into one `ipu_param` (0x98 B, defined independently per §3.1).
   - `bg_w=align16(w)`, `bg_h=align16(h)`, `bg_fmt=out_fmt=0x18`, `bg_buf_p = frame+0x18`.
   - Build `para` per §3.2, `buf_p` = physical address of the active DMA buffer or 0 for masks.
   - `ioctl(fd, _IO('I',106), &param)`.
   - **No** `IOCTL_IPU_BUF_FLUSH_CACHE` per frame: The bitmaps are already written back at the update,
     the frame is not dirty in the CPU.
   - Check beforehand: `x>=0, y>=0, x+w<=bg_w, y+h<=bg_h`, `w,h>=1`. In the rotated case even dimensions
     and position.
   - Discard invalid regions individually and log them rate-limited. Unlike the vendor,
     a faulty layer should not "poison" the whole pass.
   - On `ioctl < 0`: retry once. After N errors in a row, switch this group to the
     CPU backend.
5. **CPU backend:**
   - LINE/RECT: `fmt==8`, opaque, colour per §3.5, width centred.
   - BITMAP: byte ≠ 0 means Y=byte, UV=0x80.
   - Optional PIC/COVER as a fallback:
     - at update time convert to precomputed spans with α>0
     - Y, U, V and α precomputed, 4:2:0 chroma with averaged α per 2×2 block
     - α=255 as `memcpy`/`memset`, otherwise `dst += ((src - dst) * a + 128) >> 8`
   - Cache: before reading, invalidate the affected row bands (Y and UV) with dir=2, after
     writing write back with dir=1. One ioctl per band, not per row.
   - Order as in the vendor: first the CPU paths, then the IPU pass. The writeback must happen
     **before** the IPU run, otherwise a later writeback overwrites the output of the
     IPU.
6. **Timestamp FIFO** (`SetRgnAttrWithTimestamp`): in the first version treat like `SetRgnAttr`
   (already so today, `services.c:210–215`), optional later.

### 5.3 Performance considerations (one MIPS32r2 core, ~1 GHz)

- **IPU path:**
  - ~1 ioctl per stream and frame (≤ 4 regions), kernel time roughly 20–50 µs (about 60
    MMIO writes, copy_from_user 152 B, IRQ, wakeup). **[I]**
  - At 2 streams × 25 fps that is ≈ 0.1–0.3 % of a core, instead of ≈ 1 % per
    region and stream in the vendor.
  - The wait for the IPU (≤ ~0.8 ms for a full 1080p composite according to the
    cycle counter measurement in the wedge patch, measured on T20) **sleeps**, so it costs no CPU.
    It only lengthens the latency per frame. **[I]**
- **No flush per frame:** This is the biggest lever compared with the vendor. There, 1 MiB is flushed per
  region. **[I]** for the share.
- **CPU path:**
  - only pixels inside the region, plus spans without α=0 (text ≈ 20–30 % coverage)
  - row reads are sequential, prefetch helps
  - the UV plane has half as many rows
  - `memset` for opaque covers
- DDR bandwidth of the IPU: whether it streams the whole frame or only the layer rectangles is
  **[?]**. The number in the wedge patch (≈157k cycles for 1920×1088) would be implausibly fast
  at ≈200 MHz for a complete read and write of 3 MB. So probably only rectangles are processed. **[I]** Measure on the device (§5.6).

### 5.4 Risks

1. **IPU stability:** Wedges are reset within ≤ 100 ms by the Thingino patch. timps
   observes `ipu_osd error` when re-enabling channels and on ISP restart
   (timps `dev_notes/TODO.md:669–790`). Countermeasure: use the IPU only with a running stream,
   error counter, fallback to the CPU.
2. **`/dev/ipu` missing** (other kernel or configuration): CPU fallback.
3. **Concurrency with other IPU users:** The kernel serialises via a mutex. In OpenIMP
   there are no other users. The libimp CSC functions (`ipu_csc_nv12_to_*`) are not used in
   OpenIMP.
4. **Semantics of bits 24/25 and 23 and the blend rounding:** Take over the vendor values 1:1
   and confirm by test.
5. **Rotation** (`frame+0x28`, timps workarounds): In the first version limit OSD on rotated
   streams to the upper band, as in the vendor, or support the rotation
   if OpenIMP has it. To be clarified.
6. **The ABI fix** changes behaviour for callers that build against the OpenIMP headers. This probably affects
   only internal tests.
7. **Licence:** The kernel sources are GPL. For MIT code take over only the **interface** (ioctl numbers,
   field order of the 0x98-byte structure, format codes) as facts and declare the structure
   anew yourself, copy no driver code. The libimp details above are described as
   behaviour.

### 5.5 What to validate on the device

- Whether `/dev/ipu` exists (`ls -l /dev/ipu`) and the access rights.
- BGRA text and logo at all corners, also with odd x, y, w, h (not rotated).
- Colour and alpha compared with the vendor libimp: same bitmap, same position, compare the YUV
  of the frame, e.g. via a snapshot with `IMP_FrameSource_SnapFrame` or a raw
  frame dump.
- COVER with `gAlphaEn=0/1` and different `fgAlhpa` and `A`.
- 2 streams at the same time (main 1080p + sub), 4+ regions, updates at 1 Hz (timps updater) and
  a stress test with 30 Hz updates. There must be no tearing and no leak.
- CPU load per region compared with the vendor measurement: `top -H`, threads of the encoder pollers.
- JPEG snapshot contains the OSD.
- Behaviour on stream stop/start, ISP restart and day/night switch: no IPU errors.

### 5.6 Standalone test of the IPU (before integration)

A small tool `tools/t31_ipu_osd_probe.c`, analogous to `t31_hwjpeg_probe`:
1. With `DMA_AllocDescriptor` or a minimal `/dev/rmem` mapping, create an NV12 frame
   (e.g. 640×368) and a BGRA bitmap (e.g. 64×32). Fill the frame with a
   grey wedge / a ramp, the bitmap with an α gradient × colour fields.
2. Write both back (dir=1), build `ipu_param` with one layer, call `IOCTL_IPU_START`,
   invalidate the frame (dir=2) and read it.
3. Compare with the CPU reference. Derive from it:
   - the exact blend formula (rounding, `>>8` versus `/255`, premultiplied yes/no)
   - the colour matrix
   - the semantics of modes 0/1/2
   - mask colour YUV versus RGB
   - whether odd dimensions or position work
4. Measure the ioctl duration with `clock_gettime`, for 1 layer and 4 layers and for 360p and 1080p.
   From this follow the latency and the question "whole frame or only rectangle".

These values then flow into the CPU fallback so that both paths have the same colours.

### 5.7 Effort estimate

| Work package | Person-days |
|---|---|
| ABI fix (vendor layouts, enum constants, limits) | 0.5 |
| Region memory with DMA buffers, double buffering, format and size check, cover AYUV | 1.5 |
| IPU backend (param build, batching, error handling, fallback switching) | 1.5 |
| Pipeline hook (OSD group in the bind path, start/stop, T31 guard) | 0.5–1 |
| CPU backend LINE/RECT/BITMAP (+ optional PIC/COVER fallback with spans) | 1.5–2 |
| Probe tool and device validation (colour, alpha, timing, 2 streams, stress, restart) | 2–3 |
| **Total** | **≈ 7–10** |

A minimal goal "timps/prudynt text and logos visible" (PIC/BGRA and COVER only via the IPU,
without CPU paths) is reachable in **≈ 3–4 days**.

---

## 6. Open points

- Exact blend formula and rounding of the IPU, effect of `CH_BK_PREM` and of bits 23/24/25
  in the channel `para`. **[?]**
- Whether the IPU streams only the layer rectangles or the whole frame (bandwidth, latency). **[?]**
- Whether the vendor libimp does cache maintenance on the frame in the CPU line path (and why that
  is correct with cached frames). **[?]**
- Meaning of `frame+0x28` (swap of width and height, probably rotation) and
  `frame+0x3e4/0x3e8` (low-latency partial frames, ISP line counter). Not needed for OpenIMP.
  **[I]**
- `scalex`, `scaley` and `bgAlhpa` are not used in the drawing path. **[I]**
- Possible overlap of IPU scratch (pool start) and PIC_RMEM allocations in the same pool
  in the vendor libimp. Irrelevant for OpenIMP. **[?]**
