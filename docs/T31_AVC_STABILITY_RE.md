# OpenIMP T31 AVC path: causes of instability (RE report)

> Note: Audit report, created on `claude/t31-re` (commit `e7cf6b9`). The line numbers refer to this state; the fixes are on `claude/t31-avc-stability`.

Status: 2026-09-29. The working tree `/home/user/openimp-wt`, branch `claude/t31-re`, commit `e7cf6b9` was examined.
Basis of comparison:
- Binary Ninja HLIL of the stock T31 `libimp.so` 1.1.6 (`libimp.so_hlil.txt`, referred to below as "HLIL Lnnn" with address)
- Stock register trace `stock_logs.txt` (rvd, 1080p H.264 + JPEG)
- T31 `avpu` driver sources `avpu/t31/`

(`libimp.so_hlil.txt` and `stock_logs.txt` are local files and are not in the repository.)

The report contains no vendor disassembly. The behaviour is described, the locations are cited.

Marking: **[V]** verified (code/trace/HLIL read), **[I]** inferred (mechanism concluded from the code, not reproduced on the device), **[?]** open.

Not reported again (known):
- companion stage / JPEG core kick per AVC frame
- `0x8014 = 0` after AVC completion
- rmem flush workaround via the uncached `/dev/mem` alias

Note: thingino pins the upstream commit `opensensor/openimp@9cab219` in `package/openimp/openimp.mk`, not this branch. The line numbers refer to `claude/t31-re`.

---

## 0. Summary (sorted by expected impact)

| ID | Severity | Core |
|---|---|---|
| F1 | **high** | A lost or late AVC completion is never cleaned up. `Process` reports success despite the timeout, the pending entry stays, the core is not reset. `PollingStream` holds `p2_core_lock` for up to about 67 min and thus blocks **all** AVC channels. |
| F2 | **high** | The EndEncoding callback consumes the pending entry and switches off the IRQ mask and clock gate **without** checking whether the hardware has finished the frame (writeback `+0x104 == 0`). A stale or foreign IRQ thus creates a self-sustaining off-by-one chain: every frame is discarded, the stream is black or frozen. In addition there is a race between "sticky recovery" in the consumer thread and the IRQ thread. |
| F3 | **high** | The shared WAIT_IRQ thread belongs to the *first* encoder context, not to the device. If this channel is destroyed, the surviving channel loses all IRQs. The kernel's `UNBLOCK_CHANNEL` flag stays set permanently for the pooled fd. A newly started waiter spins with `EINTR` at 100 % CPU and never gets an IRQ again. |
| F4 | high/medium | `Destroy` is not serialised with the other channel on T31. It resets the shared core, masks it and gates the clock in the middle of a foreign frame. This leads to F1. There is also a use-after-free window for `g_tseries_irq_owner`, and P2 `DestroyChn`/`UnRegisterChn` run unprotected against a running `PollingStream`. |
| F5 | medium | A discarded frame (invalid size, header only, error fill) still promotes Rec→Ref and increments `frame_num`, but does not force an IDR. The decoder shows artefacts until the next GOP. The rate control learns nothing about it. |
| F6 | medium | The software JPEG encoder runs under `g_t31_encode_core_lock`. Every snapshot or MJPEG frame blocks main and sub H.264 for the duration of a CPU JPEG encode. |
| F7 | medium | `AL_EncCore_Init` (MISC_CTRL, reset, global IRQ clear `0x8018=0xffffff`, TOP_CTRL) runs per encoder session. Stock does this once per process. |
| F8 | low/medium | An IDR request is lost if `Process` aborts before the submit. |
| F9 | low/medium | In the IRQ thread there are `usleep(2000)` and a byte-wise EBSP copy from uncached memory. In addition there are 1 ms polling loops, an unthrottled `fprintf` per frame in `VBMGetFrame` and a kernel `printk` per IRQ (local driver copy). Consequence: jitter and high sys time. |
| F10–F14 | low | Timeouts on `CLOCK_REALTIME`; no check of the frame dimension; error in `prewrite_headers` unhandled; RC reset on every bitrate change; small leaks on Create/UnRegister. |

---

## 1. Findings in detail

### F1 – No recovery after completion timeout; global lock for up to about 67 min [V] (consequences partly [I])

**Locations**
- `src/t40/codec-t40.c:9864–9908` (`AL_Codec_Encode_Process`, T31 branch)
- `src/t40/codec-t40.c:9918–9981` (`AL_Codec_Encode_GetStream`)
- `src/t40/openimp_p2_encoder.c:1234` (lock), `1239` (Process), `1258–1270` (GetStream loop), `1297–1305` (release)
- `src/t40/codec-t40.c:9240–9286` (submit gates)

**Current state**
1. `AL_Codec_Encode_Process` waits under `g_t31_encode_core_lock` for up to 2000 × 1 ms for `pending == 0 && completions_drained` to change. On timeout it only logs. `ret` stays 0 from `al_codec_encode_process_impl` (`return submitted ? 0 : -1`, `:9777`). Then the lock is released.
   - There is no core reset, no masking and no emptying of the pending entry.
   - The stream buffer stays `IN_FLIGHT`.
2. P2 `IMP_Encoder_PollingStream` holds `p2_core_lock` (`:1234`) over Process **and** the GetStream loop. The loop runs `retries = max(timeout_ms, 2000)` times. Every `AL_Codec_Encode_GetStream` call internally waits 20 × `Fifo_Dequeue(…,100)` = 2 s. The sum is ≥ 2000 × 2 s ≈ **4000 s**.
   - During this time every other AVC channel waits at `p2_core_lock`.
   - The `timeout_ms` passed by the caller is ignored.
3. If the IRQ does not come at all (hardware hangs, IRQ masked, see F2/F4), then the following applies:
   - The next `PollingStream` call submits anyway, because `last_irq_id >= 0` switches off all busy gates (`:9240 ff.`).
   - A new stream buffer is allocated.
   - The pending ring now contains `[N, N+1]`.
   - The IRQ of N+1 is assigned in the callback to the **oldest** entry N (`avpu_pending_peek`, `:4480`). N has no writeback and is discarded.
   - N+1 stays pending, and `Process` runs into the 2 s timeout again.
   - After `stream_buf_count` (default 4) consecutive hangs, `avpu_acquire_stream_buffer` permanently returns −1 (`:9277–9285`). From then on nothing is ever submitted again, so no IRQ ever comes again. The channel is dead until Destroy. [I]
4. In the error path (`done:`) P2 releases the FrameSource buffer although the AVPU job formally still runs. The ISP can overwrite the buffer before a late job reads it (corrupt frame). [I]

**Why destabilising**
- Stock has no timeout construct of this kind.
- `AL_EncChannel_EndEncoding` (HLIL L72029, `0x6b82c`) assigns a completion to the channel via the *running FIFO of the core* (`getFifoRunning`).
- The scheduler starts the next channel from the completion context. In the trace `0x83f4=0x10000` → `0x83f4=1` → reset → `CL_PUSH` of the sub channel come from the same thread stack (`0x75044xxx`) as the WAIT_IRQ ioctl (`stock_logs.txt:252–288`).
- In OpenIMP, by contrast, a single lost IRQ or a foreign reset (F4) is enough to first freeze both streams for over an hour and then permanently kill one channel.

**Fix (clean-room)**
1. Timeout = recovery. On timeout in `AL_Codec_Encode_Process` (T31), still under `g_t31_encode_core_lock` and `ctx->irq_mutex`:
   - `0x8014 &= ~0x1` by read-modify-write; keep the JPEG bit, like stock `AL_EncCore_DisableEnc1Interrupt`, HLIL L72725.
   - Reset triplet `0x83f0 = 1,2,4`.
   - `0x8018 = 0x0f` (acknowledge only the core-0 bits).
   - Then `TurnOffGC`.
   - Pop all pending entries of this context and set the buffers to `FREE`.
   - `reference_valid = 0` (the next picture becomes IDR).
   - Increment a counter `ctx->submit_epoch++` (for F2).
   - Return −1 with `errno = ETIMEDOUT`, so that P2 releases the frame **after** the reset.
2. P2: limit the GetStream loop by a deadline, e.g. `deadline = now + max(timeout_ms, 2000 ms)`, instead of counting iterations. Never hold `p2_core_lock` longer than this deadline.
3. Optional: replace polling by a condvar that `avpu_complete_frame` signals. This also reduces F9.

**Verification on the device**
- Fault injection via a debug env (proposal, never implemented: `OPENIMP_T31_DROP_IRQ_EVERY` does not exist in the code), e.g. `=500`: ignore every n-th IRQ in the callback.
- Expectation after the fix:
  - a gap of at most about 2 s, then IDR;
  - the other channel keeps running;
  - `no free AVPU stream buffer` never appears.
- Before the fix: both streams freeze for minutes (`logread | grep "serialized completion timeout"`).

---

### F2 – Completion is accepted without hardware evidence → off-by-one and "black stream" [I] (mechanics [V])

**Locations**
- `src/t40/codec-t40.c:4441–4830` (`avpu_end_encoding_callback`), in particular `4546–4561`, `4784–4815`
- `src/t40/codec-t40.c:4009–4069` (`avpu_complete_frame`)
- `src/t40/codec-t40.c:4095–4209` (`avpu_try_recover_sticky_completion`), called from `GetStream` `:9972` without `irq_mutex`
- `src/t40/codec-t40.c:5130–5139` (owner selection without lock on T31)
- `src/t40/codec-t40.c:9654–9696` (owner/push)

**Current state**
- The callback looks at the oldest pending entry of the *current owner* and reads `+0x104` from the submit slot there. The CL entry is rebuilt per frame with `memset(cmd,0,512)` (`:2952`), so `+0x104` is **0** before the hardware writeback.
- With `payload == 0` ("invalid T31 entropy size") it continues anyway (`:4784–4803`, comment "Continue through normal completion"):
  - `0x8014 = 0` and `TurnOffGC` (`:4814–4815`), i.e. IRQ masked and clock gated while the frame may still be running;
  - `avpu_complete_frame` → pop, `avpu_promote_reference` (Rec↔Ref swap), `frames_encoded++`, drop, `completions_drained++`.
- Thereupon `Process` sees "completed", releases the lock, and the next frame is submitted.
- Sources of a "false" IRQ:
  1. **Late IRQ after a timeout (F1).** It lands at the next owner, because `g_tseries_irq_owner` stays set after the completion and is read without a lock on T31.
  2. **Sticky recovery.** `GetStream` calls `avpu_try_recover_sticky_completion` without `irq_mutex`. This happens after every 100 ms idle period of the FIFO, so regularly at low frame rate or at night. If frame N is completed this way before the IRQ thread got to it, the real IRQ of N counts as a "delayed real IRQ". The code comment `:4163–4166` describes exactly that. It is then assigned to the already submitted N+1.
  3. An IRQ that sits in the kernel queue between reset/unmask and push. The IRQ thread waits for `irq_mutex`. After the push it sees the new pending entry and consumes it (`:9671–9696`).
- Consequence: N+1 is discarded with `payload 0` and the clock gated in the middle of the frame. If the job does finish after all, its IRQ bit stays set because of `mask = 0` and fires immediately at the next `avpu_enable_interrupts` (`:4215`). Then N+2 is discarded the same way.
  - The chain can sustain itself. Symptom: `GetStream` permanently returns `ENODATA`, i.e. a black or frozen stream.
  - The comment in `GetStream` `:9950–9966` describes exactly this symptom and treats only the consequence.
- Side effect: After the first drop, `frames_encoded > frames_consumed` holds permanently. Sticky recovery is then off for ever (`:4155`). This is inconsistent.

**Why destabilising / comparison with stock**
- Stock gates and masks only in the scheduler path after a completion assigned via the running FIFO (HLIL L72029 ff.; L66357 `AL_EncCore_TurnOffGC` only for modules that are not running, `AL_CoreState_IsRunning`).
- `IsEnc1AlreadyRunning` (HLIL L72440, `0x6c678`) checks `0x83f8 & 2`.
- Stock has no sticky recovery.

**Fix**
1. In the T31 callback, check the writeback **first**. If `payload == 0` and no error fill:
   - treat it as *spurious* and log it;
   - **no** writing of `0x8014`/`0x83f4`, **no** pop, `return`.
   - Only a valid writeback (0 < n ≤ capacity) or an explicit error fill counts as a completion. Error fill → discard and IDR (F5).
2. Set `ctx->irq_armed = 1` per push (under `irq_mutex`). The callback acts only if `armed` and resets it. Stale IRQs with `armed == 0` are ignored.
3. Remove sticky recovery, or allow it only in the timeout recovery path (F1) under `irq_mutex` and after checking `0x83f8 & 2 == 0`.
4. Do the owner selection in the IRQ thread on T31 under `g_tseries_irq_host_lock`, as already on T41 (`:5129–5176`). Set the owner to `NULL` after a valid completion.

**Verification**
- 12 h main + sub at night or low fps.
- The counters for `invalid T31 entropy size`, `completion without pending stream` and `ENODATA` must stay 0.
- Also test an artificial delay in the IRQ thread (e.g. `usleep(50 ms)` before the dispatch, via env). Before the fix: drop chain. After the fix: only latency.

---

### F3 – IRQ waiter hangs on the first encoder; `UNBLOCK` is permanent in the kernel [V]

**Locations**
- `src/t40/codec-t40.c:8425–8452` (host = `&enc->avpu` of the first AVC channel)
- `:7277–7293` (Destroy: `irq_thread_running = 0`, `UNBLOCK`, join)
- `:7331–7338` (host to `NULL`)
- `:5099–5107` (waiter: `EINTR` → `continue`)
- Kernel `avpu/t31/avpu_main.c:211–215` (`unblock_channel` sets `chan->unblock = 1`, never reset) and `:219–236` (`wait_irq` returns `-EINTR` immediately when the flag is set)
- `avpu/t31/avpu_ip.c:36–39` (only **one** open channel per device, otherwise `-ENODEV`)
- `src/device_pool.c` (one shared fd, refcount)

**Current state**
- The waiter thread runs on the context of the first AVC encoder that calls `Process` (often sub).
- If this channel is destroyed (e.g. reconfig of only this stream), the following happens:
  - `UNBLOCK` on the **shared** fd, join, host = `NULL`.
  - The surviving channel has no waiter any more. Every frame runs into F1.
- If the destroyed channel is created again, the following applies:
  - `g_tseries_irq_host == NULL`, so a new waiter starts on the same fd.
  - The fd was never closed, because the other codec (or HW JPEG, `t31_hwjpeg_setup` `:7677`) holds the pool refcount. So `chan->unblock` stays 1.
  - `WAIT_IRQ` returns immediately with `EINTR`, the thread does `continue`: busy loop at 100 % CPU, no IRQ is ever delivered, both streams are dead.

**Stock**
- `AL_Board_Create` creates the waiter once per process in `EncoderInit`:
  - HLIL L84629, `0x823e0` → `AL_Codec_Create` L78957, `0x78d90` → `AL_Board_Create` `0x3612c`.
- It is ended only in `AL_Codec_Destroy` (L79025, `0x78ffc`, from EncoderExit).
- `LinuxIpCtrl_Destroy` (L39126, `0x36098`): `UNBLOCK` → join → delete mutex → `AL_DevicePool_Close`.
- `WaitInterruptThread` (L39062, `0x35e28`) **ends** on every ioctl error, also on `EINTR`; only `perror` is suppressed for `EINTR`.
- So the lifetime of the waiter is device lifetime, not channel lifetime.

**Fix**
- Make the waiter a process-wide singleton with a refcount (codecs + HW JPEG), analogous to `avpu_t41_irq_host_get/put` (`:5191–5262`), ported to T31. Create it at the first user, destroy it at the last.
- `UNBLOCK` only if the pool refcount safely becomes 0 afterwards and the fd is actually closed. Only then does the kernel free `chan`.
- Waiter: on `EINTR`, leave the loop if `!irq_thread_running`. Otherwise, after N consecutive immediate `EINTR`, log an error and sleep (protection against spinning).
- Optional in the driver (thingino-controlled): reset `unblock` in `wait_irq` after delivery (one-shot).

**Verification**
- Let main and sub run. Destroy and re-create only the waiter host channel (prudynt/raptor reconfig of one stream, or a test program).
- Before: the other stream freezes, `top` shows a thread at about 100 % (sys).
- After: both keep running.

---

### F4 – `Destroy` disturbs the running frame of the other channel; UAF window; P2 teardown race [V] (impact [I])

**Locations**
- `src/t40/codec-t40.c:7390–7411` (`AL_Codec_Encode_Destroy`: only T41 serialises via `openimp_core_acquire`)
- `:7183–7271` (mask, ack, reset triplet, GC off on the shared core)
- `:7331–7351` (owner cleared only **after** DMA release, then `irq_mutex` released)
- `src/t40/openimp_p2_encoder.c:1000–1028` (`DestroyChn` without `p2_core_lock`, without waiting for an active `PollingStream`)
- `:1051–1068` (`UnRegisterChn` sets `source_channel = -1` while a `PollingStream` still needs it for `ReleaseFrame`)

**Current state and consequences**
1. When sub is destroyed while main is running (main is in `PollingStream` with a frame in the core), `0x8014 = 0`, the `0x8018` ack, `0x83f0 = 1,2,4` and `TurnOffGC` run. This kills the main frame. Main then hangs in F1.
2. The IRQ thread reads `g_tseries_irq_owner` without a lock (`:5137`). Destroy clears the owner only at the end and then releases `irq_mutex` and `enc`. An owner that has already been read leads to a lock or callback on freed memory or on unmapped CL rings (crash). The window is small because masking happens first; only IRQs already queued in the kernel are affected.
3. `DestroyChn` checks only `raw_stream`/`source_frame`. A `PollingStream` of the same channel that is just running has not set these yet. Consequence: `AL_Codec_Encode_Destroy` in parallel to `Process`/`GetStream` → use-after-free.
4. `UnRegisterChn` during a running poll leads to `IMP_FrameSource_ReleaseFrame(-1, …)`. The VBM buffer is never returned, the capture pool shrinks with each reconfig.

**Stock**
- `AL_EncChannel_ScheduleDestruction` (HLIL L72230, `0x6c230`) only marks the channel. The scheduler does the destruction when the channel is idle (`IsChannelIdle`, L66330 ff.).
- No core reset per channel. [V for the marking; that no reset happens: [I]]

**Fix**
- T31 `Destroy` under `g_t31_encode_core_lock`; additionally take `p2_core_lock` in P2 `DestroyChn` for AVC.
- Wait for its own pending entry or clear it with the F1 recovery.
- Reset/GC-off/mask only if **no** other initialised AVC context exists any more (session counter). Otherwise remove only its own callbacks and the owner.
- Clear the owner under the same lock that the IRQ thread holds during dispatch (as T41), **before** DMA and mutex are released.
- P2: counter `ch->in_poll` per channel. `DestroyChn`/`UnRegisterChn`/`StopRecvPic` wait on a condvar for 0. Copy `source_channel` locally on poll entry.

**Verification**
- Script: stream main and sub, destroy and re-create sub every second.
- Before: gaps or freeze in the main stream, occasional SIGSEGV, growing number of `in_userspace` VBM buffers.
- After: main runs through.

---

### F5 – Discarded frame without IDR enforcement; reference is promoted anyway [V]

**Locations**
- `src/t40/codec-t40.c:4038–4058` (`avpu_complete_frame`: `promote_reference` + `frames_encoded++` before the publish check; on `!queued` only `dropped_completions++`)
- `:5586–5598` (T31 refuses an invalid size)
- `:6709–6726` (refuses header only)

**Current state**
- A frame whose completion is invalid (error fill, too large, header only) is not published.
- Nevertheless the (possibly corrupt) reconstruction becomes the reference.
- `frame_num`/POC keep running, `force_next_idr` is not set.
- The decoder loses a P picture and shows smears or grey until the next periodic IDR (GOP length, often 2–4 s).
- The T31 rate control (`t31_rate_control.c`) is not fed when `completed == 0` (`:4702–4706`). A repeated overflow in detailed night scenes is therefore not countered via QP. [I]

**Fix**
- In `avpu_complete_frame` on `!queued`: `reference_valid = 0` (safer than `force_next_idr`, because the reconstruction itself may be invalid).
- On overflow (`payload > capacity`), report a substitute value "capacity × 8" to the RC, or directly QP + n.

**Verification**
- With fault injection (ignore every n-th writeback), `NALs 7 8 5` (IDR) must follow immediately afterwards.
- VLC/ffprobe without reference errors (`ffmpeg -v error -i rtsp://…`: no "reference picture missing").

---

### F6 – Software JPEG blocks the AVC core lock [V] (duration [?])

**Locations**
- `src/t40/codec-t40.c:9871` (T31 `AL_Codec_Encode_Process` takes `g_t31_encode_core_lock` for **every** codec type)
- `:9812–9819` (JPEG → `t31_hwjpeg_encode` or `HW_Encoder_Encode_Software` → `HW_Encoder_Encode_NV12_JPEG`, `src/hw_encoder.c:1199–1200`)

**Current state**
- P2 deliberately lets JPEG run outside `p2_core_lock` (comment `openimp_p2_encoder.c:1226–1232`).
- The codec layer then serialises it after all with the AVC submit and the 2 s completion wait.
- A 1080p CPU JPEG encode blocks main and sub. Consequence: stutter or missed frames on every snapshot or MJPEG frame.

**Fix**
- In `AL_Codec_Encode_Process` (T31), take the lock only if `use_hardware == 2` (AVPU AVC) or an HW JPEG job starts.
- Software JPEG without the lock.

**Verification**
- `curl /image.jpg` in a loop while RTSP runs.
- Before, visible gaps (frame interval histogram via `ffprobe -show_frames`), after, none.

---

### F7 – Init sequence per session instead of once per device [V]

**Locations**
- `src/t40/codec-t40.c:9063–9135` (on `!session_ready`: `0x8010 = 0x1000`, `0x83f0 = 1,2,4`, `0x8018 = 0xffffff`, `0x8054 = 0x80`)
- Stock trace: exactly **one** `WR 0x8010`/`0x8018`/`0x8054` (`stock_logs.txt:69, 81, 84`) for 34 CL pushes for main + sub.

**Current state**
- The second AVC channel resets the core at its first frame and clears all IRQ bits, including the JPEG core bit 4.
- Under the lock this is mostly harmless. But it loses a pending JPEG completion IRQ with `OPENIMP_T31_HW_JPEG=1`. In addition there is a symmetric reset at Destroy (F4).

**Fix**
- Static flag "core initialised" under `g_t31_encode_core_lock`. Init only for the first context, analogous to stock `AL_EncCore_Init` (once per core/scheduler).

**Verification**
- Register trace (patched `avpu.ko`) with main + sub: `0x8018 = 0xffffff` may appear only once.

---

### F8 – IDR request is lost [V]

**Location**
- `src/t40/codec-t40.c:9041` (`force_idr = __sync_lock_test_and_set(&enc->force_next_idr, 0)` at the very start)
- Early returns: `:9249` (busy skip), `:9277–9285` (no free buffer), `:9346–9353` (RC prepare), `:9472–9477` (submit slot missing), `:9679–9686` (tracking)

**Current state**
- If the submit fails, the request is consumed. New RTSP clients then wait until the periodic IDR.

**Fix**
- On every return without `submitted` and with `force_idr != 0`, set the flag again.

**Verification**
- Call `IMP_Encoder_RequestIDR` with the stream pool exhausted (delay ReleaseStream artificially). The next published picture must be IDR.

---

### F9 – Heavy work in the IRQ thread, polling, log pressure [V] (influence on stability [I])

**Locations**
- `src/t40/codec-t40.c:5609` (`usleep(2000)` in every completion)
- `:5733–5808` (EBSP copy byte by byte from `uncached_map`, the stream alias via `/dev/mem`, `:8488`)
- `:5641–5649` (the invalidation of the *cached* alias is then ineffective)
- `:9888–9897` (1 ms polling in the `Process` wait)
- `openimp_p2_encoder.c:1213–1217` (1 ms polling on `GetFrame`)
- `src/kernel_interface.c` `VBMGetFrame` (unthrottled `fprintf(stderr, "[VBM] GetFrame…")` per frame)
- `avpu_write_reg`/`avpu_read_reg` (`:572–630`, 1 KiB `posix_memalign`/`free` each per register access)
- `avpu/t31/avpu_ip.c:119` (`printk(KERN_INFO …)` in hardirq per IRQ; local driver copy, whether the thingino `avpu.ko` contains this: [?])

**Why relevant**
- On the single-core T31 this lengthens the completion-to-publish latency. For IDRs from uncached RAM that is several ms.
- It enlarges the time window for the races from F2 (sticky recovery against IRQ thread).
- It explains part of the roughly 70 % sys time measured in `prudynt-cpu-profile.md`.
- If `stderr` is attached to a logger pipe, the `fprintf` per frame can block the encoder thread.

**Fix**
- Read the payload via the cached mapping after an invalidation to `payload_end` (already present). Search word-wise for `00 00 0x` and use `memcpy` for blocks without escape.
- Replace `usleep(2000)` by a criterion: writeback `+0x104` read twice stably.
- Wait via condvar.
- Throttle the `VBMGetFrame` log.
- Use a static register buffer per thread.

**Verification**
- `/proc/<pid>/task/*/stat` before and after: share of the IRQ thread.
- Completion→GetStream latency in the profile (`OPENIMP_PROFILE_*`).

---

### F10–F14 (low)

- **F10 [V]** `src/fifo.c:148–157, 216–225` and the static `fifo_dequeue` `:4399–4406`: timed waits on `CLOCK_REALTIME`. An NTP or time-of-day jump shortens or lengthens the GetStream and queue timeouts.
  - Fix: `pthread_condattr_setclock(CLOCK_MONOTONIC)` (as already in `t31_hwjpeg_init_once` `:7579`). Reproduce semaphore timeouts relatively.
- **F11 [V]** No check whether frame `width`/`height`/`size` (`frame+0x08/0x0c/0x14`) match `ctx->enc_w/enc_h`. `fill_cmd_regs_enc1` computes the chroma address from the encoder configuration (`:3111–3125`). On a FrameSource reconfig the AVPU reads beyond the end of the buffer or with a wrong chroma position.
  - Fix: discard the frame if the size does not match or `size < nv12_size(enc_w, enc_h)`.
- **F12 [V]** `avpu_prewrite_stream_headers` can return 0 (`:2522–2536`). `Process` submits anyway. `stream_header_offset_by_buf[buf]` then keeps the value of an earlier frame, and the published AU carries a wrong slice header.
  - Fix: on 0, release the buffer and return `EIO`.
- **F13 [V]** Every bitrate, fps, GOP or QP-limit change resets the T31 RC completely (`avpu_t31_prepare_picture` `:2398–2421`). QP jumps to `iInitialQP`. `openimp_t31_rate_controller_set_bitrate` exists but is not used for T31 AVPU.
  - Fix: use `set_bitrate` for a pure bitrate change.
- **F14 [V]**
  - `AL_Codec_Encode_SetStreamBufferCount` calls `Fifo_Init` again on an already initialised FIFO (`:6957–6958`): leak plus double init of mutex, cond and semaphore on every `CreateChn`.
  - `VBMReleaseFrame` with an invalid channel after `UnRegisterChn` (F4.4).
- **[?]** The VBM ready queue delivers the **oldest** frame (`kernel_interface.c:1589 ff.`). If the channel fps is smaller than the sensor fps, latency and the number of buffers held in userspace grow (the ISP queue drains). Whether stock does "newest wins" is open.

---

## 2. Checked and unremarkable

- **Stream buffer life cycle** [V]
  - Transitions `FREE → IN_FLIGHT` (acquire) `→ READY` (pop) `→ FREE` (ReleaseStream by phys match), all under `stream_queue_mutex` (`:3913–4007`, `:10018–10070`).
  - Descriptor per slot from the fixed pool (`avpu_stream_descriptors[16]`).
  - The stream FIFO has capacity `stream_buf_count ≥` number of `READY` buffers. `Fifo_Queue(-1)` in the IRQ thread therefore cannot block in normal operation.
- **CL rings** [V]
  - Per channel its own readback and submit rings with 19 × 0x200. On T31 uncached via `/dev/mem` (`:8543–8565`).
  - Every entry is rewritten completely per frame, so the status word `+0x104` is 0 before the push. There is no reuse during a job (serial, one job per channel).
- **Cache handoffs** [V]
  - Header prefix 0x220: write cached, then WBACK (`:9383–9396`). 0x220 is aligned to 32 bytes, so it shares no cache line with the payload.
  - Payload: read via the uncached alias, then copy into a CPU-owned buffer. No more compacting in the DMA buffer.
  - Source frame: invalidation before the submit (`:8040–8069`).
  - EP1/EP2/EP3: written once at init and written back by WBACK (`:8698`, `:8783–8795`). On T31 the CPU does not rewrite them per frame.
- **Register parity per frame** [V], against `stock_logs.txt:100–122` (main) and `:266–288` (sub):
  - `0x83f4` TurnOnGC (RMW) → `0x83f0 = 1,2,4` → `0x8014 |=` Enc1 (RMW) → `0x83e0` → `0x83e4 = 2`.
  - After the completion: DisableEnc1 and `0x83f4 = …0000` TurnOffGC (`:314–319`).
  - OpenIMP matches this except for the known points (absolute `0x8014 = 0` instead of RMW `&~1`; bit 4 not set; companion stage) and F7.
  - No IRQ ack from userspace per frame, also like stock. The kernel acknowledges in hardirq (`avpu_ip.c:121`).
- **Source frame ownership on T31** [V]: Release only after the completion (`P2_CAPTURE_RELEASE_AFTER_COMPLETION`, `openimp_p2_encoder.c:56–64, 1276–1282`). The only exception is the error path (F1.4).
- **Reference promotion before the publish** [V] (`:4028–4041`). In the normal path there is no race between the Rec/Ref swap and the next submit, because Process waits for the completion.
- **Timestamps** [V]: From the DQBUF timestamp. open-tx-isp T31 delivers `CLOCK_MONOTONIC` (`driver/t31/tx_isp_module.c:1638`), normalised via `OpenIMP_P0_NormalizeMonotonicTimeStamp`. Monotonic through the FIFO order. No backward jumps found.
- **Rate control arithmetic** [V]: Saturating addition, division only by checked non-zero values, QP clamping (`t31_rate_control.c`).
- **Destroy order in the single-channel case** [V]: Quiesce → join waiter → unmap all VMAs → close fd (`:7183–7322`). This is correct for the kernel single channel (`avpu_ip.c:36`).

---

## 3. Recommended order of the fixes

1. **F3** (waiter singleton plus EINTR protection): small change, removes total hangs after a reconfig.
2. **F2** (completion only with writeback evidence, `irq_armed`, remove sticky recovery).
3. **F1** (timeout recovery, P2 deadline).
4. **F4** (serialise Destroy, P2 `in_poll`).
5. **F5 and F8** (IDR robustness).
6. **F6, F7, F9** (latency and parity).

Measured values for the device test with main + sub + JPEG over 12 h, plus a reconfig script every 60 s:

| Measured value | Target |
|---|---|
| `serialized completion timeout` | 0 |
| `invalid T31 entropy size` | 0 |
| `completion without pending stream` | 0 |
| `ENODATA` returns | 0 |
| `no free AVPU stream buffer` | 0 |
| Thread CPU of the IRQ waiter | < 5 % |
| RSS | stable |
| RTSP | without reference errors (`ffmpeg -v error`) |
