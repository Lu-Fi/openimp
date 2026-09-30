# T31-Testergebnisse, Runde 2: OpenIMP `claude/t31-all` (`dfbd90f`) und open-tx-isp `claude/t31-isp-all`

Testlauf am 2026-09-30 nach der Übergabe aus der Cloud-Sitzung. Ausgeführt von einer lokalen
Claude-Code-Session per SSH. Kamera anonymisiert (`cam-A`, `192.168.1.100`). Runde 1 (Original-
Stack, OpenIMP `3f501a2`) steht auf dem Branch `claude/t31-test-results-20260930`.

## Kopf

| | |
|---|---|
| Kamera | WUUK Y0510, Profil `wuuk_y0510_t31x_sc4336p_ssv6158`, Sensor SC4336P (2560×1440) |
| SoC | T31X, `isa: mips32r1`, `ASEs implemented:` leer (kein MXU2) |
| Speicher | `mem=78M@0x0 rmem=50M@0x4e00000` |
| thingino | `ciao` `cf463ffc3` mit `BR2_PACKAGE_THINGINO_ISP_OPEN=y`, dazu die drei T31-Patches `0004`–`0006` von open-tx-isp aus `aperto`; per komplettem OTA (`make ota`) geflasht, mit Freigabe des Menschen |
| Kernel | `3.10.14__isvp_swan_1.0__` |
| ISP-Modul geflasht („alt") | open-tx-isp `e92166b` + thingino-Patches `0001`–`0006` |
| ISP-Modul Test („neu") | open-tx-isp `claude/t31-isp-all` `97cccfa2` + thingino-Patches `0001`–`0006`, md5 `03b40e861c0724309e098a1c7cbfcf96` |
| libimp geflasht | OpenIMP `3f501a2` (Runde 1) |
| libimp Test | OpenIMP `claude/t31-all` `dfbd90f`, md5 `8b48664e0c0252c99625d18340f9a274`, per Bind-Mount |
| Streamer | timps v1.9.28, für die Tests von Hand mit einer Kopie der Konfiguration in `/tmp` |
| Main/Sub | H.264 1920×1080 / 640×360, 15 fps |

Regeln wie im Testplan: nur diese Kamera, Dateien nur in `/tmp`, Bind-Mount, keine Kamerabilder
im Repo (nur lokal angesehen, mit Einverständnis).

---

# Phase 1: neue libimp, geflashtes open-tx-isp

## C. Regression mit FrameSource-Zyklen – **bestanden**

Dateien: [`p1-c/`](p1-c/).

### 25 FrameSource-Zyklen

Wie in Runde 1: timps ohne Variablen starten, 45 s warten, dann 25-mal Main 8 s per RTSP
abrufen und trennen, jeweils danach 2 s Sub, 5 s Pause
([`p1-c/cycles-25.txt`](p1-c/cycles-25.txt), Log [`p1-c/run-cycles-25.log`](p1-c/run-cycles-25.log)).

| | Ergebnis |
|---|---|
| FrameSource-Enables Main / Sub | **26 / 25** (in Zyklus 19–23 hielt ein WebRTC-Client der WebUI Main 140 s lang offen, deshalb dort keine Zyklen) |
| `allocation failed` / `rmem out of memory` | 0 / 0 |
| ffmpeg-Fehler Main | 0 in allen Zyklen |
| timps-PID | durchgehend dieselbe |
| Bildprüfung | 1. und 16. Frame nach jedem Neuverbinden lokal angesehen: vollständige Bilder, kein Blockmüll. Die numerischen Abweichungen in Zyklus 2–4 sind das Einregeln der Belichtung nach dem Start, ab Zyklus 19 eine echte Szenenänderung (PTZ-Position und offene Tür). |

**Befund, altes Modul:** Einmal fror der Main-Stream nach einem Tag/Nacht-Wechsel ein und
erholte sich erst durch die Recovery von timps:

```
13:46:08.423 [INF] HAL_ING      control image.running_mode=1
13:46:16.383 [INF] HAL_ING      control image.running_mode=1        (re-assert, Stream läuft)
13:46:17.003 [WRN] HAL_ING      chn0: PollingStream idle (rc=-1, miss#1) - encoder emits no frames
13:46:21.507 [ERR] HAL_ING      chn0: encoder dead after 10 consecutive misses - forcing a framesource disable/enable cycle to recover (recovery attempt 1/5)
```

Danach lief der Stream wieder (Recovery-Versuch 1 reichte). Das ist das Einfrieren nach einem
Tag/Nacht-Wechsel, das `claude/t31-isp-all` beheben soll; Vergleich in Phase 2 (T2, L5).

### Gegenprobe `OPENIMP_RMEM_NO_REUSE=1` – **bestanden**

[`p1-c/cycles-noreuse.txt`](p1-c/cycles-noreuse.txt), [`p1-c/run-noreuse.log`](p1-c/run-noreuse.log):
Der alte Fehler kommt zurück, nach 6 Enables:

```
[DMA] rmem out of memory: requested 6266880, used 49426432 of 52428800, largest free block 3002368, 20 allocations
13:57:05.622 [ERR] HAL_ING      framesource 0: EnableChn failed (attempt 1)
…
13:57:31.313 [INF] MAIN         teardown complete - exiting
```

timps beendet sich sauber (kein Zombie wie in Runde 1 auf dem Stock-Treiber).

Nebenbei: Die Kamera war danach ~40 min von meinem Rechner aus nicht erreichbar, lief aber
durch (Uptime unverändert, DHCP-Verlängerungen im syslog). Das war ein Netzproblem zwischen
Rechner und Kamera, kein Kamerafehler.

### 10-Minuten-Lauf

Main+Sub per RTSP live dekodiert, Snapshot alle 10 s, `top`
([`p1-c/c3-summary.txt`](p1-c/c3-summary.txt), [`p1-c/run-c3.log`](p1-c/run-c3.log)):

| | Ergebnis |
|---|---|
| Main / Sub | 8891 / 8890 Frames in 600 s (15 fps), kein Intervall < 140 außer dem angeschnittenen Ende |
| Dekodierfehler | 0 |
| Snapshots | 56/56, Ø 358 KB, Ø 0,53 s |
| S2-Messung (30 s Frame-Zeitstempel Main) | Median 67 ms, p99 73 ms, **max 80 ms**, 0 Intervalle > 100 ms |
| `[ERR]`/`[WRN]` | 1× `jpeg chn3: PollingStream idle (miss#1)`, ohne Folgen |
| timpsd-CPU | 13–32 % (Software-JPEG-Snapshots im Messfenster) |

Die Frame-Abstände sind auf open-tx-isp deutlich gleichmäßiger als in Runde 1 auf dem
Stock-Treiber (dort max. 101 ms ohne Snapshot-Last).

## Code-Review: HW-JPEG als Standard? (Review durch eine zweite Claude-Instanz, nur lesend, Basis `dfbd90f`)

Anlass: Mit Software-JPEG liegt timpsd bei Snapshot-Last deutlich über der Original-libimp
(Runde 1, E: 66,9 % gegen 19,4 %), mit `OPENIMP_T31_HW_JPEG=1` gleichauf (18,5 %).

Empfehlung: **Standard mit Bedingungen.** Vorher:

1. *(mittel)* `src/t40/codec-t40.c:7859-7869`: Beim Nachwachsen des Stream-Puffers nach einem
   Overflow wird der alte kohärente Puffer vor der neuen Allokation freigegeben. Scheitert die
   ~2,2-MB-Allokation aus Kernel-Lowmem (Fragmentierung, vgl. A1 in Runde 1), schaltet
   `disable("DMA allocation failed")` (`:7961`) den Pfad dauerhaft ab. Fix: erst neu allokieren,
   bei Fehler alten Puffer behalten; besser Stream/EP1 aus rmem wie der AVC-Pfad.
2. *(mittel)* Start-Race: HW-JPEG prüft nur, ob der IRQ-Waiter läuft (`:7935`), die globale
   AVC-Init passiert erst im ersten AVC-Process (`:9335-9341`). Ein früher JPEG-Job kann drei
   Timeouts sammeln und den Pfad dauerhaft abschalten (`:8035`). Fix: vor der AVC-Init den Frame
   in Software kodieren, nicht abschalten.
3. *(Testbedingung)* HW-JPEG schaltet die Companion-Stage immer ab (`:8114-8122`). Vor dem Umschalten
   Test C mit Zyklen und 10-min-Lauf mit `OPENIMP_T31_HW_JPEG=1`.

Minimaler Umbau: `codec-t40.c:7784-7788` und `openimp_p2_encoder.c:231-235` gleichlautend auf
„an, außer `OPENIMP_T31_HW_JPEG=0`" umstellen; Doku von opt-in auf opt-out.

Weitere Punkte: Alle übrigen Abschaltgründe sind dauerhaft, aber ohne Flattern; `disable` sollte
als Fehler geloggt werden. Der größere Zeitstempel-Jitter mit HW-JPEG kommt daher, dass jeder
JPEG-Job (~10–15 ms inkl. 3-MB-Kopie) den gemeinsamen Core-Lock hält; für 1 Snapshot/s unkritisch,
für MJPEG spürbar. EP1 und Stream-Puffer kommen aus `/dev/avpu` (Kernel-Lowmem), nicht aus rmem.

**Zombie-Hänger aus Runde 1:** kein Bezug zu HW-JPEG. `IMP_FrameSource_DisableChn` macht
STREAMOFF vor dem Join des Tick-Threads (`src/framesource/framesource_tseries.c:1993-2005`); die
innere Schleife ruft DQBUF ohne `running`-Prüfung erneut auf (`:1019-1046`). Im Stock-tx-isp
dreht dieses DQBUF nach STREAMOFF im Kernel. Commit `70c42bc` (fd vor Pool-Freigabe schließen)
behebt das nicht und war im getesteten Stand schon enthalten. Vorschlag: auf T31 zuerst den
Thread kooperativ stoppen und joinen, dann STREAMOFF. Auf open-tx-isp trat der Hänger nicht auf.

## S1. H.264-Dauerlauf – **bestanden** (78 min, ohne Hänger)

`dfbd90f` auf dem geflashten open-tx-isp, keine Variablen, 14:57–16:15. Main und Sub dauerhaft per
RTSP live dekodiert, Snapshot alle 10 s. Gekürzt wegen einer Pause des Menschen (Abfahrt), sonst
2 h. Das Rohlog wurde auf der Kamera minütlich gefiltert und geleert (die Pro-Frame-Debugzeilen
von OpenIMP ergeben sonst ~25 MB/h in `/tmp`). Dateien: [`p1-s/`](p1-s/).

| | Main | Sub |
|---|---|---|
| Frames in 78 min | 69397 (15,0 fps) | 69393 (15,0 fps) |
| Verbindungsabbrüche / 10-s-Intervalle mit 0 Frames | 0 / 0 | 0 / 0 |
| 10-s-Intervalle < 140 Frames | 2 (135, 139) | 0 |
| Dekodierfehler | 0 | 0 |

- Snapshots **444/444** HTTP 200, Ø 0,49 s, max. 0,89 s.
- Zähler (syslog und gefiltertes Log): `completion timeout`, `without size writeback`,
  `completion found without IRQ`, `keeps returning EINTR`, `size written back but core still
  running`, `picture larger than the stream buffer`, `[ERR]`, `[WRN]`: **alle 0**.
- Speicher (`top`, belegt inkl. Cache): 36,2 MB nach dem Start, danach konstant 39,8–40,3 MB.
  `/tmp` konstant bei 3 %. timpsd-CPU aus den 10-min-Proben: 0–33 % je nach Snapshot im Fenster.
- Kein Hänger, der Recovery-Pfad wurde also nicht ausgelöst.
- Nach dem Lauf Stopp in 0,3 s, Bind-Mount gelöst, `S95timps` normal gestartet.
