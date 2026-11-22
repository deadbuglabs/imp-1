# IMP-1 firmware

ESP32-S3 firmware for **IMP-1**, the Deadbug Labs portable hardware
exploration device. Twelve tools on a frozen C API (`include/imp_api.h`),
one common shell, partial-redraw UI on a 172x320 ST7789 display.

## Tools

Sub-GHz · NFC · IR · BadUSB · GPIO · U2F · Archive · Flash Lab ·
Security Auto-Audit (passive) — plus RFID / iButton / 1-Wire as guarded
stubs that land on IMP-2 hardware.

## Build status

Clean build (PlatformIO, ESP32-S3):

- **0 errors, 0 warnings**
- RAM 41.2% · Flash 43.9%
- Full detail: [BUILD_REPORT.md](BUILD_REPORT.md)

```sh
pio run
```

## Caveats

- **U2F host interop is unverified** — the CTAP1/ECDSA engine compiles and
  is structurally complete; it has not yet been tested against a live host.
  It is the #1 bring-up verification item.
- **The boards are not yet fabricated.** Main board v13 (wider
  respin): 17 unconnected nets remain, with 0 clearance violations
  and 0 shorts; the remaining 17 are accepted bodge wires, one frozen
  net, and fence-proven items measured in the routing log. The NFC
  board is DRC-clean and the IR board's routing is done. Gate 3
  (PCB integrity) has not passed yet. This firmware is ready to
  flash the day a main board exists.
- Sub-GHz capture is FIFO-derived (the current board revision does not
  connect CC1101 GDO0/GDO2); true edge capture is an IMP-2 item.

## Sponsors

<a href="https://www.pcbway.com"><img src="https://pcbwayfile.s3.us-west-2.amazonaws.com/web/240304/PCBWay10BadgeContestLOGOdownload.svg" alt="PCBWay" height="44"></a>

**[PCBWay](https://www.pcbway.com)** — 3D-printing sponsor: a USD 30
3D-print credit (no expiry; splits across two orders), held until the
boards are fabricated and fit-checked, then it prints the cases.

Project site: [deadbuglabs.github.io](https://deadbuglabs.github.io)

— Deadbug Labs
