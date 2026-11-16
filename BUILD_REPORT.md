# IMP-1 Firmware — Build Report

**Date:** 2026-11-16 · **Project:** `files/firmware/imp1-fw/` · **Target:** ESP32-S3-WROOM-1-N8R2
**Verdict: CLEAN BUILD — SUCCESS, zero errors, zero warnings.**
RAM 41.2% (134,968 / 327,680 B) · Flash 43.5% (1,454,929 / 3,342,336 B app partition)
Toolchain: PlatformIO 6.2.0 (cadvenv), espressif32@6.10.0, Arduino-ESP32 2.0.x framework,
board `esp32-s3-devkitc-1`, partition `default_8MB.csv`. ~7,400 lines of firmware source.

This is the first REAL firmware for IMP-1: the IMP-Forge drafts were design
documents; everything under `imp1-fw/` is compiled code written against the
Builder's Pack contract (`include/imp_api.h`, frozen) and UI spec v0.1.

## Per-module status

| Module | File(s) | Status |
|---|---|---|
| pins.h | `include/pins.h` | ✅ Frozen v5 map verbatim from the pack §3 (all CS lines incl. CC_CS IO18, ST25_CS IO47, ST25_IRQ IO48). IO45 deliberately has no constant — code that wants it gets nothing. |
| imp_api.h | `include/imp_api.h` | ✅ Contract frozen from pack §7–9: all families, error enum, app manifest + `IMP_APP_REGISTER`. |
| Shared SPI bus | `src/imp_bus.cpp` | ✅ One bus (MOSI 9 / SCLK 10 / MISO 11), mutex for multi-transaction sequences, never held across a wait. |
| Display driver | `src/drivers/disp_st7789.cpp` | ✅ Hand-written ST7789V3 driver (no external libs): 172×320 portrait, GRAM x-offset 34, strip rendering (no framebuffer), per-device 40 MHz transactions. Partial redraw is structural, not aspirational. |
| UI layer | `src/imp_ui.cpp` | ✅ Spec v0.1 template: header (title + radio icons + battery), mascot box, content slot (list/values), lower text strip, status, progress, footer legend. Every call cache-checked — repaints only changed strips. Lists scroll a 12-row window over up to 16 items. Confirm is a modal that repaints what it covered. |
| Input | `src/imp_input.cpp` | ✅ Encoder quadrature in ISR (detent events), push = OK, BOOT short-press = BACK, 4×4 keypad scan (respects GPIO claims), event queue dispatched from the pump — callbacks never run in ISR. **Panic combo = BOOT + encoder push held 300 ms** → flag → executed synchronously in `imp_delay_ms` (blank, LED/buzzer off, outputs off, radios off, deep sleep; wake on push/BOOT via ext1). |
| System | `src/imp_system.cpp` | ✅ Battery via BATT_ADC ×4 divider math, WS2812 via RMT ch 0, buzzer via LEDC, cooperative `imp_delay_ms` (input pump + panic + buzzer stop), deep sleep/reboot. |
| Storage | `src/imp_storage.cpp` | ✅ microSD via Arduino SD on the shared bus (CS IO3), per-tool folders auto-created, list/read/write/remove. |
| GPIO core | `src/imp_gpio.cpp` | ✅ Claim policy per pack §11: driver-owned pins refuse `IMP_ERR_BUSY`, IO45/not-broken-out pads refuse `IMP_ERR_NO_HW`, keypad matrix pins claimable. |
| App registry | `src/imp_registry.cpp` | ✅ 12 tools registered; `requires` checked against build-1 fitted hardware (subghz/nfc/ir/flash/sd/wifi/ble/badusb/usb/gpio/u2f fitted; rfid/ibutton/onewire NOT). |
| Shell | `src/main.cpp` | ✅ Boot silhouette-flame fade, home list (imp left, list right, pose per selection), **Settings always last**, idle drift after 8 s, deep sleep on timeout (default 120 s, Settings-adjustable), settings persisted to `/settings.cfg`. |
| Sprites/font | `src/sprites_data.cpp`, `include/imp_font.h` | ✅ GENERATED from the real assets (`files/static/mascot-1bit/`, bare red imp) at 80×100 1-bit by `tools_gen/gen_assets.py`; 5×7 font generated the same way. No invented art. |
| CC1101 driver | `src/drivers/cc1101.cpp` | ✅ Real register driver: FREQ computed from Hz (26 MHz xtal), band validation → `IMP_ERR_RANGE`, PARTNUM/VERSION probe → `IMP_ERR_NO_HW` when absent, RSSI conversion, panic hook. Capture = FIFO packet-mode at 4.806 kBaud with an RSSI gate (−88 dBm) — **see limitation L1**. |
| Sub-GHz tool | `src/tools/tool_subghz.cpp` | ✅ Band picker (915 labelled strong band; other bands show "Fitted antenna is 915 MHz"), live RSSI, RX poll loop, save/load `.sub` (Flipper-shaped text), TX behind confirm. |
| ST25R3916B driver | `src/drivers/st25r3916b.cpp` | ✅ Register/direct-command driver per DS12484 (SPI mode 1, space-B regs, IRQ poll on IO48). ISO14443A REQA/WUPA + anticollision cascade, NTAG read/write/dump. Chip-ID probe accepts both 3916 (ic_type 5) and the fitted **B** variant (6) — a strict check would have failed the real board. MIFARE Classic data ops return `IMP_ERR_IO` (Crypto1 auth not implemented — poll/UID work). Receiver tuning registers left at SET_DEFAULT: antenna matching is D-04 bring-up work, not hard-coded (per pack). |
| NFC tool | `src/tools/tool_nfc.cpp` | ✅ Poll loop, type/UID/ATQA/SAK display, pages 0–3 hex, dump to SD with progress, NTAG page-4 test write ("IMP1") behind confirm + re-read verify. MIFARE shows plain-words "needs card keys / not in this build". Field-off on exit via the documented `imp_nfc_poll(nullptr)` convention (the frozen API has no NFC stop call). |
| IR driver | `src/drivers/ir_rmt.cpp` | ✅ RMT ch 1 TX (carrier from signal, 33% duty) / ch 2 RX (20 ms idle threshold = end-of-signal), NEC decode with RAW fallback, `.ir` text format (Flipper-spirit). TSOP active-low handled. |
| IR tool | `src/tools/tool_ir.cpp` | ✅ Learn → auto-save to `/ir/`, library picker, send behind confirm. |
| Flash driver | `src/drivers/extflash.cpp` | ✅ RDID (EF 40 17 / C2 20 17 accepted; unknown ID → writes refuse `IMP_ERR_IO`), page-split writes, 4 KB sector erase, WIP polling in delay slices, WP# lock detected via SR1 BP bits → `IMP_ERR_LOCKED`. |
| Flash Lab tool | `src/tools/tool_flashlab.cpp` | ✅ RDID-first (unknown ID = stop screen), chunked dump with real progress, dump-first rule enforced before erase/write, write test = 16-byte pattern + re-read verify, LOCKED shown as "WP# jumper is locked (J4)". |
| BadUSB driver | `src/drivers/badusb.cpp` | ✅ USBHIDKeyboard typing + combo parser, streaming DuckyScript interpreter (REM/STRING/STRINGLN/DELAY/DEFAULT_DELAY/REPEAT/combos), USBMSC storage mode = 64 KB RAM disk with a built FAT12 image. Serial mode present-but-no-type per contract shape. |
| BadUSB tool | `src/tools/tool_badusb.cpp` | ✅ Mode picker, payload picker, run + test-type both confirm-gated, honest on-screen note that USB mode persists until reboot/mode change. |
| U2F tool | `src/tools/tool_u2f.cpp` | ⚠️ **Compiles; real CTAP1 engine; host-interop UNVERIFIED.** Full CTAPHID (INIT/PING/MSG/WINK/ERROR/CANCEL/KEEPALIVE), REGISTER + AUTHENTICATE APDUs, mbedTLS P-256 ECDSA, user-presence = encoder OK press with 10 s timeout, counters persisted. Attestation cert is hand-DER-encoded self-signed (SDK ships no x509write) — structurally real, untested against a live host; strict FIDO checkers may reject any self-signed root. Key handles use XOR+HMAC wrapping (documented v1 gap vs AES-GCM). **#1 bring-up verification item.** |
| Net driver | `src/drivers/net.cpp` | ✅ Wi-Fi scan (RF-measured, `network_reported=false`), async watch (2 s sweeps, per-sweep callback, absence = dropout record), open-network connect (credentialed join is Settings-provisioned later — no PSK in source), BLE scan. Panic hook stops watch/radios. |
| Archive tool | `src/tools/tool_archive.cpp` | ✅ Folder/file pickers over the six tool folders, detail (size measured by reading — API has no stat), delete behind confirm, BACK-only no-SD screen. |
| Auto-Audit tool | `src/tools/tool_audit.cpp` | ✅ **Passive only**, as specified: Wi-Fi + BLE inventory with detail screens, watch-my-AP logging to `/audit/watch.log`, 6-item camera resilience checklist persisted to SD, report writer. No deauth, no jamming, no connection attempts — none exists in the code. |
| RFID tool | `src/tools/tool_rfid.cpp` | ✅ Guarded stub: registry refuses entry (`requires=rfid` unmet, D-15); if ever entered, plain-words not-fitted screen, BACK exits. |
| iButton tool | `src/tools/tool_ibutton.cpp` | ✅ Guarded stub, same pattern (D-02). |
| 1-Wire tool | `src/tools/tool_onewire.cpp` | ✅ Guarded stub, same pattern (D-02). |

## Clean-build log tail (verbatim)

```
Checking size .pio/build/imp1/firmware.elf
Advanced Memory Usage is available via "PlatformIO Home > Project Inspect"
RAM:   [====      ]  41.2% (used 134968 bytes from 327680 bytes)
Flash: [====      ]  43.5% (used 1454929 bytes from 3342336 bytes)
Building .pio/build/imp1/firmware.bin with action:
  "$PYTHONEXE" "$OBJCOPY" --chip esp32s3 elf2image --flash_mode ${__get_board_flash_mode(__env__)} --flash_freq ${__get_board_f_image(__env__)} --flash_size 8MB --elf-sha256-offset 0xb0 -o $TARGET $SOURCES
Building .pio/build/imp1/firmware.bin
esptool.py v4.5.1
Creating esp32s3 image...
Merged 3 ELF sections
Successfully created esp32s3 image.
========================= [SUCCESS] Took 98.80 seconds =========================
```

Final confirmation build after the last fix: SUCCESS, **0 errors, 0 warnings**.
Binary: `.pio/build/imp1/firmware.bin` (1,455,344 B).

Integration fixes applied centrally (4, all in delegated code, none touched the
API contract): MSC callback names collided with USBMSC.h typedefs → renamed;
BLE `getNative()` return type in this SDK is `uint8_t(*)[6]` → fixed;
`s.ui` vs `s->ui` in two audit/archive helpers → fixed; generated font header
guard macro collided with its own height macro → renamed in generator + output.

## Decisions made where the sources were silent

1. **Framework**: the pack's build loop (§12) targets ESP-IDF under proot; the
   tasking specified a PlatformIO ESP32-S3 project. Built on PlatformIO +
   Arduino-ESP32 framework (which is ESP-IDF underneath) — fastest path to a
   real, flashable binary from this bench; the `imp_*` C contract is
   framework-independent, so an ESP-IDF-native port later touches only the
   driver layer. *Flagged as the one structural deviation from the pack.*
2. **Panic combo keys** (spec open item): BOOT (IO0) + encoder push (IO37)
   held together 300 ms. Both are always-wired inputs; no keypad dependency.
3. **Panic execution**: ISR-level *detection*, execution inside
   `imp_delay_ms` (worst case one tool-loop slice, ~20 ms) — deep sleep from
   raw ISR context is not safe on this stack; the effect (instant blank,
   silent, radio-off device) is identical.
4. **Flash Lab is a tool** (pack §2 lists it among build-1 tools; the tasking
   list named 11 modules). Included as the 12th tool; menu total 13 entries
   with Settings — hence the list scrolling window.
5. **Poll "not ready" convention**: `imp_subghz_rx_poll` / `imp_ir_learn_poll`
   return `IMP_ERR_BUSY` while waiting (the enum's documented "retry"
   meaning); `imp_nfc_poll` returns `IMP_ERR_IO` for an empty field.
6. **NFC field-off**: `imp_nfc_poll(nullptr)` ends the session (documented in
   the driver) because the frozen API has no `imp_nfc_stop`.
7. **Menu order** = registry (link) order, not alphabetical; Settings is
   forced last by the shell regardless. Cosmetic; a sort is a one-line
   shell change if a fixed order is wanted.
8. **GPIO tool claim set**: keypad matrix pins only (everything else on the
   frozen map is driver-owned or a strap). The tool's picker still lists a
   few owned pins so the refusal path is visible on screen, per pack §11.
9. **Value rendering**: `imp_ui_value` slots are keyed by label (up to 6),
   matching the pack template, which shows Battery once and Turns per-loop.
10. **BadUSB/U2F USB reality**: this framework enumerates USB once per boot;
    the first personality effectively wins and mode switches may need a
    re-plug. Documented in code and on the BadUSB screen.
11. **No `imp_system_uptime_ms()`** in the frozen API: the audit report is
    stamped "no RTC in build 1"; watch-log lines carry driver-side `now_ms`
    from the watch callback, which the contract does provide.

## Known limitations (honest list)

- **L1 — Sub-GHz capture fidelity.** GDO0/GDO2 are not connected on the
  frozen board, so there is no hardware edge-timing pin. Capture is
  FIFO-derived at 4.806 kBaud (208 µs/bit) with an RSSI gate. Fine for
  OOK remote replay at common baud rates; it is not a logic analyser.
  True edge capture needs a GDO wire — an IMP-2/board-rev item.
- **L2 — MIFARE Classic** data operations need Crypto1 authentication;
  poll/UID/dump-header work, block read/write return `IMP_ERR_IO` with the
  reason on screen. NTAG is fully supported.
- **L3 — U2F host interop unverified** (see module table). Transport and
  crypto are real; the hand-rolled attestation cert and self-signed root
  are the risk. Verify against a live host at bring-up before relying on it.
- **L4 — ST25 receiver tuning** registers are at datasheet defaults;
  D-04 in-situ antenna tuning at bring-up sets the real values and the
  measured read range gets recorded then (per pack).
- **L5 — SD + display + radios share one SPI bus** by frozen design; the
  SD library's transactions interleave at call granularity. Long dumps
  yield in slices; a pathological card could still stutter the UI. Bench
  behaviour to confirm at bring-up.
- **L6 — Settings persistence and U2F secrets live on the SD card** in
  plain files (build-1 bench reality). No secure element exists on the
  board; flagged, not hidden.

## What remains before this flashes to real hardware

1. **Boards.** Fabrication is still gated (v5 gates + the fabrication go-ahead). The
   firmware is ready to flash the day a main board exists; nothing here
   blocks on firmware.
2. **D-08 display gate**: the J3 module-side PH2.0 pin order is still a
   layout-stage measurement. The driver assumes the pack §4 order
   (CS/DC/RST/SCLK/MOSI/BL). If the measured order differs, only
   `pins.h`/the J3 wiring change — no tool code (that is what the API
   is for).
3. **First flash** over USB-OTG per pack §12 (`pio run -t upload` with the
   board on native USB); remember the IO46 note — if BOOT entry misbehaves
   with the IR board mated, unmate J12 and retry.
4. **Bench bring-up sequence** (pack §12 hooks, in order): USB enumeration
   → display first paint + encoder/BACK sanity → SD cold-boot check
   (IO3 strap, 8–10 boots) → CC1101 register read + radiated check against
   a known 915 MHz source → flash RDID (expect EF 40 17 or C2 20 17) →
   IR TX→RX loop on the bench pair → NFC in-situ tune (D-04) with read
   range recorded in mm → U2F against a live host (L3) → BadUSB against a
   sacrificial host.
5. **IO46 cold-boot matrix** (bring-up test 9 from the v5 package) with the
   IR board mated and unmated.
6. **Wi-Fi update path** (pack §12: iterate over Wi-Fi after first flash)
   is not yet implemented — `imp_wifi_connect` exists; an OTA/update tool
   is post-bring-up work.

*No AI, no audio, no laser anywhere in this build — per the build-1 order.
The freed I2S GPIOs (4/5/6/21) are unassigned; IO4 is BATT_ADC per the
frozen map, as the pack has it.*
