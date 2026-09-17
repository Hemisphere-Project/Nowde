# USB MIDI IN endpoint stall on slave nodes — analysis and plan (2026-09-17)

Field failure: slave Nowde nodes (AtomS3 Lite, v2.0.3) stop being heard by their Raspberry Pi after
~2 h; the Pi kernel logs `usb 1-1.2: urb status -32` (-EPIPE = STALL handshake on the node's MIDI
IN endpoint) at ~7000/s until the device is de-/re-authorized; the node's uptime continues across
the cure, so the ESP32-S3 never crashed. Hub task: `nowde#t-032`. This document is the firmware-side
study asked for on 2026-09-17, written read-only from the code on dev37 (nothing flashed, nothing
committed). Where a fact cannot be established from the code on this machine, it says so.

Companion facts already recorded in the hub (`37Projects/projects/biennale-2026-module-radar/log/2026.md`,
15:10 entry) and in the field runbook (`Runbooks/biennale-MBA/README.md`, `bin/usbfix`).

---

## 0. What the code actually is (established facts)

### 0.1 USB stack in the `atoms3` builds

| Layer | What | Where |
|---|---|---|
| Framework | Arduino-ESP32 **3.3.2** on ESP-IDF **5.5.0** (`framework-arduinoespressif32-libs` `5.5.0+sha.07e9bf4970`) | `~/.platformio/packages/framework-arduinoespressif32*/package.json` |
| USB stack | **TinyUSB 0.18.0** headers (`tusb_option.h:33-35`), prebuilt as `libarduino_tinyusb.a`; the DWC2 device driver in it is **Espressif's own copy** (`components/arduino_tinyusb/src/dcd_dwc2.c` in esp32-arduino-lib-builder, per the archive's debug strings), **not present as source on this machine**. Its object (`dcd_dwc2.c.obj`) was disassembled instead, see 1.x. | `framework-arduinoespressif32-libs/esp32s3/lib/libarduino_tinyusb.a` |
| Mode | `-DARDUINO_USB_MODE=0` (OTG/TinyUSB), `-DARDUINO_USB_CDC_ON_BOOT=0`; composite **MIDI + CDC**; slave-mode (CPU-driven) FIFOs, no DMA | `platformio.ini` env `atoms3` (master runs `atoms3-bcast` = same + `NOWDE_MEDIASYNC_BROADCAST=1`) |
| Descriptors / endpoints | CDC reserves OUT 3, IN 4, IN 5 (`esp32-hal-tinyusb.c:797-802`, `USBCDC.cpp:33`: notif **0x85**, data **0x03/0x84**). MIDI then takes the first free IN paired with an existing OUT → **MIDI IN = 0x83**, MIDI OUT = **0x04** (`esp32-hal-tinyusb.c:875-903`, `USBMIDI.cpp:28-34`). MIDI endpoints are **BULK** (`usbd.h:346-348`, `TUD_MIDI_DESC_EP … TUSB_XFER_BULK`). | |
| Controller limits | ESP32-S3 DWC2: 7 endpoints, **5 IN endpoints with a TX FIFO**, 1024-byte FIFO RAM (`dwc2_esp32.h:44-48`); Arduino caps at `CFG_TUD_NUM_EPS 6 / CFG_TUD_NUM_IN_EPS 5` (`esp32-hal-tinyusb.h:45-46`). EP5 IN (CDC notification) is the one that "is actually not linked to FIFO and not used" (`esp32-hal-tinyusb.c:635`) — an Espressif-specific hack inside the prebuilt DCD. | |
| Class FIFOs | **`CONFIG_TINYUSB_MIDI_TX_BUFSIZE 64` / RX 64** (= 16 USB-MIDI packets each), CDC 64/64, compiled into the prebuilt lib — **not changeable from the sketch** | `framework-arduinoespressif32-libs/esp32s3/qio_qspi/include/sdkconfig.h:503-515` |
| TinyUSB task | `usbd` task, `xTaskCreate` (**no core affinity**), priority `configMAX_PRIORITIES-1` (= 24), stack 4096, runs `tud_task()` forever | `esp32-hal-tinyusb.c:762-767, 846` |
| USB ISR | `esp_intr_alloc(ETS_USB_INTR_SOURCE, ESP_INTR_FLAG_LOWMED, …)` from the task that calls `USB.begin()` → `setup()` → Arduino `loopTask` on **core 1** (`CONFIG_ARDUINO_RUNNING_CORE 1`) → **USB ISR on core 1** | `dwc2_esp32.h:100-107` |
| Arduino USB events | `tud_mount_cb / tud_umount_cb / tud_suspend_cb / tud_resume_cb` → `ARDUINO_USB_*_EVENT`; `USB.onEvent()` exists; **the firmware registers no handler** (grep: no `USB.onEvent` in `src/`) | `USB.cpp:100-130, 192-197` |
| No `USB.end()` | `ESPUSB` has `begin()` only; `tud_deinit`, `tud_disconnect`, `tud_connect`, `usbd_edpt_clear_stall`, `usbd_edpt_busy`, `usbd_edpt_stalled` **are exported** by the prebuilt `usbd.c.obj` (`nm`) → usable for recovery | |

### 0.2 Firmware MIDI send path (single writer, already)

- Producers only enqueue (`usbOutMidi`, `usb_out.cpp:110-119`: `xQueueSend(…, 0)` — never blocks; `xQueueSendFromISR` in ISR context). Queue = **1024 packets** (`usb_out.cpp:12`). Multi-packet frames are kept contiguous with a mutex (`usbOutLock`, `usb_out.cpp:121-131`).
- Producers: `midiSendTimeCode` from **espnowTask (core 1, prio 10)** every 33 ms (`main.cpp:256-266`, `midi.cpp:44-74`); `midiSendCC100 / midiSendFullFrame / Start/Stop` from the **ESP-NOW receive callback = WiFi task, core 0, prio 23** (`esp_now_handlers.cpp:83-86` → `receiver_mode.cpp:150-257`); HELLO / CONFIG / RUNNING_STATE / LOG / OTA_ACK from the SysEx handler, which runs in the **MIDI task** (`midi.cpp:120-188`, `sysex.cpp:190-209, 700-818`).
- Single consumer: **MIDI task, core 0, prio 24**, every **1 ms** (`main.cpp:180-187, 386-394`): `midiProcess()` → `usbOutPump()` (`midi.cpp:123`; `usb_out.cpp:133-159`) → up to 64× `MIDI.writePacket` → `tud_midi_packet_write` (`USBMIDI.cpp:187-189`), then `MIDI.readPacket` → `tud_midi_packet_read` loop.
- Back-pressure policy (`usb_out.cpp:142-154`): a failed write is retried in order for **200 ms** (`USB_STALL_DROP_MS`, `usb_out.cpp:15`), then **`xQueueReset` drops everything queued**. Nothing ever blocks on USB; nothing ever stalls an endpoint.
- Host-link bookkeeping: `lastHostRxTime` on any packet from the host (`midi.cpp:129`), `hostLinked()` = < 5 s (`nowde_config.h:120`, `nowde_state.cpp:94-96`); the slave answers each host probe (HPlayer2 `QUERY_RUNNING_STATE` every 2 s) with a 22-byte HELLO (`sysex.cpp:203-207`).
- CDC: enumerated for the 1200-bps re-flash touch, **carries no data** in the production build (`usb_out.h:6-16`, `nowde_config.h:29`: `DEBUG_SERIAL = usbLog` → ring buffer → LOG SysEx frames only while `SET_LOG 1`).

### 0.3 The IN stream, quantified (why "slave" ≠ "master" on the wire)

TinyUSB flushes on every `tud_midi_packet_write` when the IN endpoint is idle, and batches the rest of a burst into the next transfer when it is busy (`midi_device.c` `write_flush`, 0.18). So per 33 ms MTC burst of 8 quarter-frames the slave does **~2 IN data transactions** (4 B, then 28 B), i.e. **~60 data transactions/s**, plus HELLO (8 packets = 32 B) every 2 s, CC#100 1/s, LOG frames if enabled. The master's IN carries RUNNING_STATE bursts at 1 Hz (~17 packets per connected slave → ~3 full 64-B transactions/s) and nothing else. Over 2 h a slave completes **~430 000** IN transfers, the master **~20 000**. The host side polls bulk IN continuously in both cases (snd-usb-midi keeps `INPUT_URBS 7` × 64-B URBs pending): **the node's cadence does not change how often the Pi polls, only how often a poll returns data**.

### 0.4 Host side, as recorded

- Kernel: **6.18.38-v7+ (rpi-update)** on RastaOS 7.3-Biennale26, Raspbian **Buster userland** (`37Projects/knowledge/rpi/rastaos-images.md:94,189`; `Nowde/docs/BENCH.md` step 4) — not 5.10. Host controller driver: the downstream **`dwc_otg`** with FIQ (no `dtoverlay=dwc2` anywhere in Pi-tools; a commented `dwc_otg.speed=1` in `Pi-tools/bootstrap/bootstrap-raspbian-pi4.sh:202`). The node is a full-speed device behind the Pi 3B+'s LAN9514 high-speed hub (`1-1.2`) → every transaction is a **split transaction through the hub's TT**.
- `snd-usb-midi` (upstream `sound/usb/midi.c`, fetched today, unchanged in shape since forever): `-EPIPE` is in **neither** `case` list of `snd_usbmidi_urb_error` → `default:` → `dev_err("urb status %d")` + `return 0` → `snd_usbmidi_in_urb_complete` **resubmits immediately**. Only `-EPROTO/-ETIME/-EILSEQ` get the 100 ms `error_timer` (`ERROR_DELAY_JIFFIES = HZ/10`). No `usb_clear_halt` anywhere in the MIDI IN path. **No upstream fix exists as of today's master.**
- `dwc_otg` (`rpi-6.18.y`, `dwc_otg_hcd_intr.c`, fetched): `-DWC_E_PIPE` (→ `-EPIPE`) is produced in **one** place, `handle_hc_stall_intr`, i.e. the host channel's `HCINT.STALL` bit — a STALL handshake received by the controller (from the device directly, or relayed by the TT in the complete-split). The FIQ FSM path (`FIQ_NP_SPLIT_LS_ABORTED … hcint.b.stall`) funnels into the same handler. The handler completes the URB, resets the data toggle and halts the channel; it does **not** latch a "stalled" state — the next URB is a fresh bus transaction. Therefore 25 minutes of -32 at 7000/s means **something answered STALL to ~10 million consecutive IN tokens**, or the host driver's per-endpoint state (QH/channel) was corrupt and completed URBs without a real bus transaction.

---

## 1. Ranked root-cause hypotheses

Ranking after the coordinator's evidence of the afternoon (W4 stalled 2 s into a `systemctl daemon-reload`; W5 within 20 s of a 26-year `date -s` + `hwclock -w`; W6 with a per-minute journalctl/MIDI-probe job running; 5/5 stalls on dense-IN slaves, 0 on the master). Both sides stay on the table because two facts pull in opposite directions: the **triggers are host CPU events**, but the **victims are the data-bearing nodes only** (the master's Pi polls the same way and is under the same kind of load).

### H1 — HOST: `dwc_otg` (FIQ split-transaction machinery) leaves the endpoint's host-side state reporting STALL after an IRQ-latency spike — rank 1 (≈45 %)

- For: all three timed stalls coincide with Pi-side CPU/IRQ contention; `dwc_otg` on Pi 0-3 is exactly the driver for which the FIQ exists because it cannot tolerate latency on split transactions; the cure is host-driven and only sends `SET_CONFIGURATION 0/1` to the device (no port reset — `authorized` is `usb_deauthorize_device → usb_set_configuration(-1)` then re-enumerate + `SET_CONFIGURATION 1`), which also **destroys and recreates the host's QH/channel state**; the node kept its uptime and answered at once.
- Against: the master's Pi polls IN just as continuously (NAK-only) and has never stalled. If H1 is right, the bug must live in the **data-completion** path of the non-periodic split FSM (60×/s on slaves vs ~3×/s on the master), not in polling per se. A pure host-state corruption also has to explain why STALL and not `-EPROTO`.
- Confirms: the cure ladder (§2, tier A: driver unbind/bind alone cures) or a device-side register snapshot showing `DIEPCTL3.STALL = 0` while the Pi logs -32; the same node never stalling on a PC host under identical stream (§2).
- Refutes: `DIEPCTL3.STALL = 1` read on the node at stall time, or `CLEAR_FEATURE(ENDPOINT_HALT)` being what cures it.

### H2 — DEVICE: the node's DIEPCTL(3).STALL bit really is set — rank 2 (≈25 %)

- What the code says: in TinyUSB 0.18 and in the prebuilt Espressif DCD, the STALL bit is written **only** by `edpt_disable(…, stall=true)` (`dcd_dwc2.c`, quoted from 0.18.0; disassembly of the prebuilt: `dcd_edpt_stall → edpt_disable$isra$0`, no other writer). `dcd_edpt_stall` is called from `usbd_edpt_stall` — reached only from the standard `SET_FEATURE(ENDPOINT_HALT)` handler — and from the **EP0** control-failure path in `tud_task_ext` (`dcd_edpt_stall(rhport, 0)` / `(0x80)`), which cannot touch EP3. **`midi_device.c.obj` and `cdc_device.c.obj` reference no stall function at all** (`nm -u`). So no *known* software path halts 0x83; the coordinator's "TinyUSB halts IN on FIFO overrun when the host stops polling" **does not exist**: when the host stops reading, `tud_midi_packet_write` simply returns false once the 16-packet class FIFO is full and the pump drops the queue after 200 ms (`usb_out.cpp:142-154`) — a silent gap, never a halt.
- What would still set it: (a) a `SET_FEATURE(HALT)` from the host — Linux never sends one in normal operation (only `usbtest`); (b) corruption of `_dcd_data`/`xfer_status` or a stray full-register write to `DIEPCTL3` (`edpt_schedule_packets` does `dep->diepctl = depctl.value`, a read-modify-write of the whole register); (c) undocumented DWC2/ESP32-S3 behaviour. None can be confirmed from this machine.
- Note for later: **`usbd_edpt_clear_stall(0, 0x83)` is exported** by the prebuilt usbd → if H2 is the case, the node can un-halt itself (§3).
- Confirms: register snapshot with `STALL=1`; cure ladder tier B (CLEAR_HALT from the Pi) fixes it while tier A does not.

### H3 — DEVICE: IN endpoint stuck-NAK (transfer armed, TXFE never fires, `busy` never released) — rank 3 (≈15 %), but this is the *2026-09-04 bench symptom*, not today's

- Mechanism: `dcd_edpt_xfer` sets `diepempmsk |= (1<<ep)` after arming; the ISR's `handle_ep_irq` clears it with `diepempmsk &= ~(1<<ep)` on TXFE-done and on XFRC. In the prebuilt DCD (disassembly, `dcd_int_handler` call order), **`usbd_spin_lock` is taken in `dcd_edpt_xfer`/`dcd_edpt_xfer_fifo`/`dcd_edpt_close_all` and, inside the ISR, only around the bus-reset block (`memset` + `dfifo_device_init`); `handle_ep_irq` and its `diepempmsk` RMWs run outside any lock.** With the writer on **core 0** (MIDI task pinned there; the unpinned `usbd` task can be there too) and the ISR on **core 1**, a `taskENTER_CRITICAL` on core 0 does not exclude the ISR on core 1, so a lost update remains possible across cores (the same-core case is covered). A lost *set* → the armed transfer never gets its data → the endpoint NAKs forever, `tx_ff` fills, the pump drops every 200 ms → **silence** on the host (no -32). A lost *clear* → a TXFE interrupt with `packet_count == 0` → the handler clears the mask itself → self-heals.
- Why it is not today's headline: a stuck-NAK endpoint produces no STALL handshake; the Pi would see nothing, not -32. It **is** the "CDC log silent until reboot / IN transfers lost" of the 2026-09-04 bench (`usb_out.h:12-14`, `BENCH.md` "USB link deaths"), which the single-writer refactor made rarer but did not remove — the writer still calls `dcd_edpt_xfer` from core 0 against an ISR on core 1. Keep it in scope for 2.0.4 (the same watchdog catches it: host probes arrive, answers never leave).
- Confirms: snapshot shows `DIEPCTL3.EPENA=1, STALL=0, DIEPTSIZ3.PKTCNT>0, diepempmsk bit clear`, `usbd_edpt_busy(0x83)` true for seconds, no -32 on the Pi.

### H4 — HUB: the LAN9514 transaction translator misbehaves (stuck buffer / stale complete-split) — rank 4 (≈10 %)

- The TT relays the device's STALL; per USB 2.0 §11.17 the TT itself answers NAK/NYET/ERR, not STALL (spec text not re-read today — treat as "to be confirmed"). `dwc_otg` never issues `Clear_TT_Buffer` (EHCI does after split errors). A TT stuck on that endpoint would be expected to yield NYET/ERR (→ `-EPROTO`), not `-EPIPE`, and `authorized 0/1` sends nothing to the hub. Low, kept because the Pi 3B+'s hub is in the path of every transaction and only a device-side snapshot can exclude it (`STALL=0` on the node + `-32` on the Pi + no XFRC completions on the node = hub or host).

### H5 — DEVICE: bus glitch → spurious USBRST / suspend seen by the node — rank 5 (≈3 %)

- A false reset would put the node at address 0 with all endpoints inactive → the Pi would log `-EPROTO`/`-71` descriptor errors, not a clean `-32` stream; a suspend is only an event here (`USB.cpp:117`, nothing acted on). Does not fit. Kept only because the firmware currently **ignores** these events, which §3 fixes anyway.

### H6 — BOARD: AtomS3 (LCD, master W3) vs AtomS3 Lite (LED, five slaves) — rank 6 (≈2 %) — answer to the coordinator's question

Board and role are **100 % confounded in this fleet**: role AUTO resolves from the board id (`main.cpp:309-317`, `uiDetectBoard`, `ui.cpp:196-206`; M5Unified detects the Lite from a GPIO pattern and the AtomS3 from the GC9107/ST7735 panel probe). What the firmware does differently per board:

| | AtomS3 (LCD) — master | AtomS3 Lite — slaves |
|---|---|---|
| UI tick (5 Hz, `ui.cpp:17, 214-253`) | `drawStatusPage()` full 128×128 redraw over SPI (`ui.cpp:69-135`), backlight PWM (LEDC) | `rgbLedWrite(35, …)` (`ui.cpp:157-165`) → Arduino RMT TX channel + **RMT ISR on core 1, same core as the USB ISR** (`esp32-hal-rgb-led.c:37, 94`, `rmtWrite … RMT_WAIT_FOR_EVER`, ~35 µs blocking) |
| Build | `atoms3-bcast` (= `atoms3` + broadcast relay) | `atoms3` |
| Board manifest, flash, PSRAM, USB PHY, partitions, descriptors, tasks, priorities | identical (`board = m5stack-atoms3`, 8 MB, no PSRAM) | identical |
| Power | LCD + backlight, ~+30 mA | LED only |
| USB IN traffic | trickle (RUNNING_STATE 1 Hz) | dense (§0.3) |
| USB OUT traffic (host→node) | dense (MEDIA_SYNC 10 Hz + poll 1 Hz) | trickle (probe 0.5 Hz) |
| ESP-NOW | mostly TX | RX 10 Hz + beacons + mesh clock, callbacks in the WiFi task on core 0 |

Nothing board-specific touches USB, clocks or power domains; the only board-specific periodic activity near the USB ISR is the Lite's 5 Hz RMT write, which is microseconds and cannot produce a STALL. Ranked last. **The one experiment that separates board from role** costs no reflash: swap roles over MIDI with the stored `SET_ROLE` (`nowde-cli.py set-role slave` on the AtomS3, `set-role master` on one Lite; both persist in NVS and are reverted the same way; optionally OTA the `-bcast` bin onto that Lite so the relay shape stays the fleet's), physically swap the two boxes' positions (so each keeps a Pi with the matching host traffic), run ≥ 4 h (2× the observed MTBF) with `usbfix` logging. AtomS3-as-slave stalls → role/traffic; the Lite-as-master stalls → board is irrelevant and the master position was protecting it; neither → inconclusive, extend.

---

## 2. Host vs device — the separating experiments

Three instruments, in order of cost. The first can be in the field tonight and needs no firmware.

**A. The cure ladder in `usbfix` (host side, decides on the next natural stall).** On a storm, try tiers in order and log which one stops the -32 flood (1 s of `dmesg` silence):

1. **Driver unbind/bind only** — `echo 1-1.2:1.2 > /sys/bus/usb/drivers/snd-usb-audio/unbind` then `bind` (the MIDI interface number is 2 in this composite: CDC 0-1, MIDI 2-3; confirm with `lsusb -t`). No bus traffic to the node except the driver re-probing (GET descriptors already cached, endpoint re-enable). **Cures → HOST (H1)**: the device endpoint was never halted; the Pi's per-endpoint state was.
2. **`CLEAR_FEATURE(ENDPOINT_HALT)` on 0x83** via usbfs (pyusb: `dev.detach_kernel_driver(2); dev.clear_halt(0x83); dev.attach_kernel_driver(2)`; needs `python3-usb`/`pyusb` on the Pi — not verified installed). **Cures while tier 1 did not → DEVICE (H2)**: TinyUSB's `usbd_edpt_clear_stall → dcd_edpt_clear_stall` cleared a set STALL bit.
3. **`authorized 0/1`** (today's cure) → SET_CONFIGURATION 0/1: re-opens every device endpoint fresh (`dcd_edpt_close_all` + `edpt_activate`, whole `DIEPCTL` rewritten, FIFOs flushed) *and* recreates host state. Cures while 1 and 2 did not → device endpoint/FIFO state without the STALL bit (H3-like), or hub.
4. Port reset (`usbreset`/`uhubctl` if present) — last.

**B. The node's own view at stall time (firmware 2.0.4-diag, decides at the next stall too).** When the host-silence watchdog (§3) fires, snapshot before doing anything: `USB0.in_ep_reg[3].diepctl` (bits: EPENA 31, STALL 21, NAKSTS 17, USBAEP 15), `.diepint`, `.dieptsiz` (PKTCNT/XFRSIZ), `.dtxfsts`, `USB0.diepempmsk`, `USB0.gintsts`, `USB0.dsts` (SOFFN advancing = bus alive), `USB0.dctl`, plus `usbd_edpt_busy(0, 0x83)`, `usbd_edpt_stalled(0, 0x83)`, `tud_suspended()`, `(bool)USB`, `tud_midi_mounted()`, `millis()`, the pump's `stalledSince`, and a count of MIDI IN completions in the last 10 s (`midid_xfer_cb` is not hookable, but `MIDI.writePacket` success/failure per second is). Register names from `soc/usb_struct.h` (`usb_dev_t USB0`, `in_ep_reg[7]`, `gintsts 0x14`, `dctl 0x804`, `dsts 0x808`, `diepempmsk 0x834`). Store to RTC-retained RAM + NVS and emit as a LOG frame on the next link (the IN endpoint is dead at that moment) and as the `[USBWD]` line in the boot banner if a reboot followed. Reading:
   - `STALL=1` → **device** halted (H2).
   - `STALL=0, EPENA=1, PKTCNT>0, empmsk bit 0`, no completions → device stuck-NAK (H3) — the Pi would show silence, not -32; if it shows -32 anyway, look at the hub (H4).
   - `STALL=0`, completions still happening at ~60/s while the Pi logs -32 → **host** (H1): the Pi is not even talking to us.

**C. Same stream, different host (soak, 4 h each).** The single experiment the coordinator asked for: one Lite in slave role, plugged into the dev37 laptop (xHCI, no TT if plugged directly into a USB 2/3 root port... a full-speed device on an xHCI root port needs no TT; on a USB 2 external hub it does), receiving the garden master's mesh (or a bench master via `nowde-cli.py play 3 -d 20 -t 14400` on a second node), watched with `nowde-cli.py watch` + `dmesg -w | grep -c 'urb status'`, with `SET_LOG 1` on to add IN traffic; in parallel the same-batch Lite on a Pi 3B+ (RastaOS 7.3, HPlayer2 or `nowde-cli watch`) under the §5 host stressors. Laptop never stalls in ≥ 8 h while the Pi does → host-specific (H1/H4). Both stall → device (H2/H3). Complement: a **non-Nowde** dense USB-MIDI source into the same Pi (e.g. the DevKitC-1 build of this firmware = MIDI-only composite, or any keyboard streaming MTC at 30 fps) — stalls → host, but be aware this only tells about *that* device's response to the same host misbehaviour.

**Board vs role**: the SET_ROLE swap of H6.

---

## 3. PREVENTION candidates in firmware (2.0.4)

Ordered by expected value against **both** sides. Each with its cost to the Pi servo (the Drifter chase-lock ticks on every `nowde.qf` (one per 8 quarter-frames = 66 ms) or `nowde.ff` identically: `handle_timecode` → `drifter.tick(round(seconds, 2))`, `HPlayer2/core/interfaces/nowde.py:813-830`; both events are wired at `:326-327`).

| # | Change | Where | Effect on exposure | Cost |
|---|---|---|---|---|
| P1 | **Fewer, fuller IN transfers**: make each 8-QF burst leave as **one** 32-byte transaction instead of 4 B + 28 B. The public API flushes on the first write; the trick is to write the 8 packets while the endpoint is *busy* — i.e. right after a preceding transfer was queued. Practical form: the pump sends the 8 QF as a group only when `usbd_edpt_busy(0,0x83)` is false **after** first queuing a 1-packet "leader" — brittle. Better form: **switch the slave to MTC full-frame at 15 Hz** (10-byte SysEx = 4 packets = 16 B, one transaction) and keep quarter-frames as the default for MillluBridge/v1.2 hosts; an NVS/SysEx switch `SET_MTC_MODE` (0 = QF, 1 = FF@15 Hz, 2 = both). | `midi.cpp:44-74, 76-90`, `main.cpp:261-265`, `sysex.cpp` new cmd | −50 % transfers (60 → 30/s) with FF@15 Hz; −80 % with FF@10 Hz | **Zero for HPlayer2** (same tick rate and 10 ms rounding either way; FF also cures the "position jump waits for the next QF cycle" case). Breaks nothing on v1.2 hosts if QF stays default. Only worth it if §2 points at the device or the host's data-completion path. |
| P2 | **MIDI-only production composite (no CDC)** — env `atoms3-nocdc`: do not construct `USBSerial`, drop `UsbLog::begin()`'s `USBSerial.begin()`. Removes EP 0x03/0x84/0x85 (the FIFO-less EP5 hack) and every CDC control request (`SET_LINE_CODING` etc. from anything probing `/dev/ttyACM*`). This is also the DevKitC-1 shape that has run for a year on AnnaTV without this failure (different host, different traffic — weak evidence). | `nowde_state.cpp:7`, `usb_out.cpp:181-184`, `platformio.ini` | Removes 3 endpoints and one Espressif-specific code path from the device; irrelevant to a pure host bug | Lose the 1200-bps re-flash touch (OTA over MIDI stays; ROM mode needs the button). `board_upload.use_1200bps_touch` must go for that env. |
| P3 | **Act on USB bus events**: `USB.onEvent(...)` for `ARDUINO_USB_STOPPED/SUSPEND/RESUME/STARTED`: on STOPPED/SUSPEND flush the MIDI queue and `stalledSince` (no ghost stream on resume), on STARTED (mount) queue a HELLO. Cheap hygiene; not a cure. | `main.cpp` setup, `usb_out.cpp` | none | none |
| P4 | **Pump hysteresis**: today a write failure is retried in order for 200 ms then `xQueueReset` (`usb_out.cpp:142-154`); under a host that pauses ~250 ms (a `daemon-reload`!) this discards a full second of MTC and the HELLO answer that was due — harmless to the Pi (it decodes the next cycle) but it hides the failure signature. Keep the drop for *stale* MTC, but never drop the *last* HELLO/OTA_ACK: mark SysEx answers as "sticky" or re-arm a HELLO on the next host packet (`hostResumed` already exists, `midi.cpp:127`). | `usb_out.cpp` | none | none |
| P5 | **Cross-core hardening (H3)**: pin the `usbd` task to core 1 (same core as the USB ISR) so every `dcd_edpt_xfer` issued from `tud_task` is same-core with the ISR, and make the MIDI task **not** call `dcd_edpt_xfer` from core 0: either pin the MIDI task to core 1 too (it is a 1 ms poller; core 1 has the espnowTask at prio 10 — fine), or route writes through `tud_task`'s completions only. The Arduino HAL creates `usbd` with `xTaskCreate` (no affinity) — it can be pinned after the fact with `vTaskCoreAffinitySet`? Not in IDF; instead create the MIDI task on core 1 and accept the WiFi task on core 0 alone. | `main.cpp:386-394` | closes the only unprotected RMW race found | MIDI task now shares core 1 with espnowTask/UI; at prio 24 with 1 ms sleeps it still preempts everything |
| P6 | **Lower CC/HELLO chatter**: HELLO every 2 s is the host's choice (`PROBE_INTERVAL = 2.0`); CC#100 1/s is `CC100_REPEAT_INTERVAL_MS`. Both negligible (≤ 5 packets/s). Not worth touching. | — | — | — |

Not possible from the sketch: enlarging `CFG_TUD_MIDI_TX_BUFSIZE` (64 B, baked into the prebuilt lib) or fixing the DCD race itself. Those need a rebuilt `libarduino_tinyusb.a` (esp32-arduino-lib-builder) or moving the project to ESP-IDF + `esp_tinyusb` — a 2.1 decision, not a 2.0.4 one.

---

## 4. RECOVERY in firmware — a USB self-watchdog

Goal: the node notices its own dead link and repairs it in seconds, without a Pi-side watchdog, and without a reboot when a cheaper step works. Everything below uses symbols that **exist in the prebuilt objects** (`tud_disconnect`, `tud_connect`, `tud_suspended`, `tud_mounted`, `usbd_edpt_busy`, `usbd_edpt_stalled`, `usbd_edpt_clear_stall` — `nm` on `usbd.c.obj`; `dcd_disconnect/dcd_connect` in `dcd_dwc2.c.obj`) plus `esp_restart`. `USB.end()` does not exist; `tud_deinit` does but re-`tusb_init` through the Arduino HAL is not supported (`tinyusb_is_initialized` latch, `esp32-hal-tinyusb.c:810-814`) — so "re-init USB" = soft disconnect/connect, and the last resort is a restart.

**Detector** (run from espnowTask's 10 ms tick, `main.cpp:198-275`, or a small prio-5 task; never from the MIDI task):

```
mounted   = (bool)USB && tud_midi_mounted() && !tud_suspended()
hostQuiet = (millis() - lastHostRxTime) > USBWD_HOST_SILENCE_MS      // 8000: host probes every 1-2 s
wasTalking= host sent >= 3 packets in the previous 60 s               // avoids firing on a Pi that is simply off
pumpDead  = writes have been failing for > 2000 ms (stalledSince != 0)   // export from usb_out.cpp
epStuck   = usbd_edpt_busy(0, 0x83) continuously > 2000 ms, or USB0.in_ep_reg[3].diepctl STALL bit
fire      = mounted && wasTalking && hostQuiet && (pumpDead || epStuck)
```

Guards: `otaInProgress` → never fire (OTA traffic keeps `lastHostRxTime` fresh anyway); at most one action per 30 s; per-boot escalation counter.

**Ladder** (each tier logged with the §2.B snapshot):

- **L0 — snapshot** to RTC RAM + `[USBWD]` LOG frame; also bump a `usbwd` byte for the HELLO trailer (there is already a 2.0.4 trailer change queued, `nowde#t-031` stream age; add `usb_wd_count(1)` next to it, wire-compatible).
- **L1 — un-halt in place** (only if `STALL=1` or `usbd_edpt_stalled`): `usbd_edpt_clear_stall(0, 0x83)`; then re-arm by queuing a HELLO. Cost: ~0 s of gap. Fixes H2 outright.
- **L2 — soft re-enumeration**: `tud_disconnect()` (DCTL.SDIS drops the D+ pull-up → the Pi sees a disconnect), wait 200 ms, `tud_connect()`. The Pi re-enumerates; TinyUSB re-opens all endpoints on `SET_CONFIGURATION` (fresh `DIEPCTL`, flushed FIFOs — exactly what today's `authorized 0/1` achieves, but from the device). HPlayer2 notices the port name vanish (`CONNECTION_CHECK_INTERVAL 2 s`) and relinks (`PORT_LOOKUP_INTERVAL 5 s`) → **3-8 s of MTC gap**, the Drifter freewheels (already proven benign by `usbfix-deploy test`). Fixes H2, H3 and any device-side FIFO state; **also fixes H1 and H4** because the host tears down and rebuilds its QH/TT state on disconnect. This tier is the one that matters regardless of which side is guilty.
- **L3 — restart** after two L2 within 10 min without the host coming back: `esp_restart()` bounded by an NVS counter (reuse the `resyncrb` pattern, `main.cpp:161-173`, `storage.cpp:71-85`), cleared when the host talks again. 5-10 s gap; state (role, layer, LR) is in NVS, the master resends position within 100 ms of the mesh coming back.

False-positive analysis: a Pi that stopped HPlayer2 (or a laptop bench without a host program) makes `hostQuiet` true but `wasTalking` false after 60 s → no action; a Pi rebooting → `mounted` false → no action; OTA → guarded. The only bad case is a host that is alive but has legitimately closed the port for > 8 s after talking: one L2 (a 3-8 s re-enumeration) then silence — acceptable.

Feasibility caveat: `tud_disconnect()` on ESP32-S3 with the internal PHY is the standard way `esp_tinyusb` re-enumerates, but the Arduino HAL's `tinyusb_device_mounted` flag will flip through `tud_umount_cb`, and `USBCDC::_onUnplugged` fires — harmless here (CDC unused). Must be bench-proven on a Pi before shipping (§5 R4).

---

## 5. HOST side (Linux) — short

- Kernel in the field: **6.18.38-v7+** (rpi-update, RastaOS 7.3-Biennale26, Buster userland), `dwc_otg` host driver. `snd-usb-midi` loops on `-EPIPE` in this kernel exactly as upstream master does (§0.4); **no module option or `usbcore.quirks` bit changes that path** (the quirk table only touches descriptors/reset behaviour). A one-line kernel patch would end the storm: add `case -EPIPE:` to the `-EIO` group in `snd_usbmidi_urb_error` (100 ms `error_timer` instead of immediate resubmit), or schedule `usb_clear_halt()` from a work item. Building `snd-usb-audio.ko` for an rpi-update kernel on a read-only rootfs is a real project (headers, `rw`, module signing off) — not for this week; worth an upstream report with today's log.
- What is realistic now: (1) keep `usbfix` but turn it into the **cure ladder** of §2.A (tier 1 is also a 100 ms cure vs a 3-8 s re-enumeration); (2) let **HPlayer2 detect and trigger**: it already sees the HELLO answers stop (`_probe` every 2 s, `_handle_sysex`) — "3 missed HELLOs while the port is open and the Drifter is freewheeling" → call the same cure (root is fine on these players) in ~6 s instead of ≤ 60 s; (3) `ATTR{power/control}="on"` udev rule for `303a:8000` (autosuspend is not implicated — HPlayer2 holds the port — but it removes one variable for the bench); (4) make sure nothing probes `/dev/ttyACM*` (ModemManager is not expected on RastaOS; `lsof /dev/ttyACM0` once to be sure); (5) journald: the storm proved rate limiting cannot save the journal — the fix is the cure speed, as the runbook already concluded.

---

## 6. Bench REPRODUCTION protocol (< 2 h target)

Setup: one Lite (slave), one master node (or the laptop as software master: `uv run tools/nowde-cli.py play 3 -d 20 -t 7200` on a second node), a Pi 3B+ on RastaOS 7.3 with HPlayer2 `biennale` profile (or `nowde-cli.py watch` in the HPlayer2 venv), the dev37 laptop as the alternate host. Both hosts log `dmesg -w | ts` and `journalctl -f -u 'hplayer2@*'`; the Pi runs `blackbox` (has the `urb=` field) and `usbfix` in **log-only** mode (`exit 0` before the reset) so a stall is observed, not cured, until the ladder is run by hand.

- **R0 — symptom on demand (10 min).** From the Pi with pyusb: `SET_FEATURE(ENDPOINT_HALT)` on 0x83 (`dev.ctrl_transfer(0x02, 0x03, 0x0000, 0x83)`). TinyUSB sets the real STALL bit (`usbd_edpt_stall`), the Pi enters the -32 storm within a second. This reproduces the **host-side consequence** deterministically and is the test bench for `usbfix`'s ladder, for HPlayer2's fast trigger, and for the firmware's L1/L2. It does not reproduce the *cause*.
- **R1 — baseline soak (2-4 h).** Slave on the Pi, master streaming, `SET_LOG 1` on the slave (adds ~100 packets/s of LOG SysEx = more IN transfers). Pass: 0 `urb status` lines, HELLO cadence 30/min unbroken. Fail: storm or silence > 10 s.
- **R2 — host-latency accelerators (the field triggers, 1-2 h).** On the Pi, in a loop every 30 s: `systemctl daemon-reload`; `date -s "+1 year"` then `hwclock -w`; three `journalctl --since -10min` scans; `stress-ng --cpu 3 --io 1 -t 20`; a python MIDI probe that opens/closes the port. Record the timestamp of each versus the first -32.
- **R3 — device-side accelerators.** From the host, `QUERY_RUNNING_STATE` at 20-50 Hz (each answered by an 8-packet HELLO → 20-50× more IN transfers than the field); toggle `SET_LOG` on/off every minute; unplug/replug every 10 min; `echo 0/1 > authorized` every 5 min; `echo auto > power/control` with the port closed for 30 s then reopened.
- **R4 — recovery proof (2.0.4-diag).** With R0, verify L1 clears the storm without re-enumeration; with a simulated dead host (kill HPlayer2 mid-stream, then `SET_FEATURE(HALT)`), verify L2 re-enumerates within 10 s and HPlayer2 relinks; verify L3 never triggers while the host is alive; run the whole thing 6 h with R2 running. Pass: no unbroken silence > 15 s, no -32 stream longer than 1 s, no restart while the host was alive.
- **R5 — host vs device soak (§2.C)**, 4 h per host, same node, same stream.

What to log on both sides: Pi — `dmesg` with timestamps, `usbmon` (`/sys/kernel/debug/usb/usbmon/1u`) around each event, `cat /sys/kernel/debug/usb/devices` (endpoint list), `blackbox` line, the exact command being run when it happened; node — `SET_LOG 1` stream captured by `nowde-cli.py watch` (`[MIDI TX] MTC` every 5 s, HELLO, `[USBWD]` snapshot lines), LED colour.

---

## 7. Implementation and rollout plan

1. **Tonight, no firmware**: `usbfix` → cure ladder (tier 1 unbind/bind, tier 2 CLEAR_HALT if pyusb is available, tier 3 `authorized`), each tier logged with its outcome. This is the host-vs-device instrument; it also shortens the gap. Runbook change, not this repo — proposed here, Thomas's to run.
2. **2.0.4-diag (1 day)**: watchdog detector + §2.B snapshot + L0/L1/L2/L3 behind `NOWDE_USBWD=1`, P3 (USB events), P4 (pump hysteresis), P5 (MIDI task on core 1), HELLO trailer bytes (`stream age` from `#t-031` + `usb_wd_count`). Unit-test on the laptop with R0; then R4 on a Pi.
3. **Field: one slave first** (W6, already has `usbfix`), via `Runbooks/biennale-MBA/bin/nowde-ota.py <bin> acked` from its Pi (`hplayer2` stopped, ~1-2 min, node reboots into the image, verdict = fresh HELLO; old image stays if `OTA_END` refuses). 24 h of blackbox + usbfix logs → read the snapshot of the next stall → this decides §2.
4. **2.0.4 final**: depending on §2 — device-side verdict: P1 (FF@15 Hz switch), P2 (`atoms3-nocdc` env, only if the snapshot implicates the CDC endpoints or the diag build never stalls with CDC removed on the bench); host-side verdict: keep L2 as the node's contribution, push the kernel report, HPlayer2 fast trigger. Tag `v2.0.4`, bins for `atoms3`, `atoms3-bcast`, `atoms3-lr` (Thomas tags, per the release doctrine).
5. **Rollout order**: W6 → W4/W5 → W1/W2 → master W3 last (`atoms3-bcast` bin; the master has never stalled, and reflashing it drops the mesh for ~10 s). Each node: stop `hplayer2@`, `nowde-ota.py … acked`, start, watch HELLO + lock for 5 min. ~10 min per node, no box opened.
6. **Risks / fallback**: a watchdog false positive causes a 3-8 s re-enumeration (bounded, logged, visible in HELLO count) — acceptable and reversible by OTA back to 2.0.3 (kept in `bin/` and `Runbooks/…/firmware/`); an OTA that fails leaves the old image (stop-and-wait, END-validated); a node that does not re-enumerate after OTA → box open, button, ROM flash (`README.md` "A node left in ROM mode"). The Pi-side `usbfix` stays on all six regardless of firmware, so the show is protected either way.

---

## 8. Open points that this machine cannot settle

- The exact Espressif `dcd_dwc2.c` used to build `libarduino_tinyusb.a` (GitHub tree of `esp32-arduino-lib-builder/components/arduino_tinyusb` shows only `include/`, `CMakeLists.txt`, `Kconfig.projbuild` today; the `src/` seen in the debug strings is not reachable by URL from here). The disassembly is the ground truth used above.
- Whether the DWC2 core can emit STALL for an *inactive* (USBAEP=0) IN endpoint — databook not available; irrelevant if the snapshot is implemented.
- USB 2.0 §11.17 TT response set (whether a TT can originate STALL) — to be re-read from the spec before leaning on H4's dismissal.
- pyusb availability on the players (for ladder tier 2 and for R0).

---

## Summary (15 lines)

1. Stack: Arduino-ESP32 3.3.2 / IDF 5.5 / TinyUSB 0.18.0 prebuilt with Espressif's own DWC2 DCD; composite CDC (0x03/0x84/0x85) + MIDI (IN **0x83**, OUT 0x04), **bulk** endpoints, 64-byte class FIFOs baked into the lib.
2. The firmware already has one USB writer (MIDI task, core 0, prio 24, 1 ms) and never blocks or stalls; a host pause only makes it drop queued MTC after 200 ms (`usb_out.cpp:142-154`).
3. "TinyUSB halts the IN endpoint on FIFO overrun" is not a code path: the MIDI and CDC class objects reference no stall function; the STALL bit is written only by `dcd_edpt_stall`, reachable from `SET_FEATURE(HALT)` or the EP0 failure path.
4. On the Pi (kernel 6.18.38-v7+, `dwc_otg`), `-EPIPE` comes from one place, `handle_hc_stall_intr` (a STALL handshake seen by the host channel), and `snd-usb-midi` resubmits it immediately — upstream master still does; no module option or quirk changes that.
5. Ranking: H1 host (`dwc_otg` FIQ/QH state after IRQ-latency events) ≈45 %, H2 device STALL bit set ≈25 %, H3 device stuck-NAK (the 2026-09-04 silent failure, an unprotected cross-core `diepempmsk` RMW confirmed in the disassembly) ≈15 %, H4 hub TT ≈10 %, H5 bus glitch ≈3 %, H6 board ≈2 %.
6. Board vs role: 100 % confounded (role AUTO from board id); nothing board-specific touches USB; the separating experiment is a `SET_ROLE` swap over MIDI (no reflash) for ≥ 4 h.
7. Host vs device, cheapest first: turn `usbfix` into a **cure ladder** (driver unbind/bind → CLEAR_HALT → `authorized`); which tier cures decides the side at the next natural stall.
8. Second instrument: a 2.0.4-diag build that snapshots `DIEPCTL3` (STALL/EPENA/PKTCNT), `diepempmsk`, `usbd_edpt_busy/stalled` when the host goes silent and reports it on relink.
9. Third: the same node, same stream, 4 h on a PC host versus the Pi.
10. Prevention worth shipping regardless: USB bus-event handling, pump hysteresis (never drop the pending HELLO), MIDI task on core 1 (same core as the USB ISR).
11. Prevention if the device is guilty: MTC full-frame at 15 Hz as a stored option (halves IN transfers, **zero cost** to HPlayer2's Drifter, which ticks on `qf` and `ff` identically) and a MIDI-only `atoms3-nocdc` env.
12. Recovery in firmware is feasible with the prebuilt stack: detector = mounted && host was talking && silent 8 s && (writes failing or endpoint busy/stalled); ladder L1 `usbd_edpt_clear_stall(0,0x83)` → L2 `tud_disconnect()/tud_connect()` (3-8 s gap, HPlayer2 relinks by itself, fixes both sides' state) → L3 bounded `esp_restart()`.
13. Reproduce the symptom on demand in 10 min with pyusb `SET_FEATURE(HALT)` on 0x83 (tests `usbfix`, HPlayer2's trigger and L1/L2); reproduce the cause with the field triggers (`daemon-reload`, `date -s`, journalctl scans, `stress-ng`) plus 20-50 Hz `QUERY_RUNNING_STATE` and `SET_LOG 1`.
14. Rollout: ladder tonight (host side, no flash) → 2.0.4-diag on W6 by acked OTA (~2 min) → read the first snapshot → 2.0.4 final → W4/W5 → W1/W2 → master last with the `-bcast` bin; fallback = OTA back to 2.0.3, `usbfix` stays everywhere.
15. Not settled here: Espressif's DCD source, DWC2 behaviour for inactive endpoints, USB 2.0 TT response set, pyusb on the players.
