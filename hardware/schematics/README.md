# IMP-1 Schematics

KiCad schematics for the three IMP-1 boards, published by Deadbug Labs under the project's file-sharing split (approved 2026-11-16).

| File | Board |
|---|---|
| `imp1-main.kicad_sch` / `imp1-main.pdf` | Main board — ESP32-S3, CC1101 Sub-GHz, display, USB-C, microSD, BQ24074 power tree (2× 18650, 1S2P), UI |
| `imp1-nfc.kicad_sch` / `imp1-nfc.pdf` | NFC/RFID front-end — ST25R3916B, board-to-board interconnect to the main board |
| `imp1-ir.kicad_sch` / `imp1-ir.pdf` | IR / iButton board — IR transmit/receive, 1-Wire |

The `.kicad_sch` files are the editable sources (KiCad 7). The PDFs are the same sheets rendered for reading without KiCad.

## What is NOT here, and why

Manufacturing files — Gerbers, BOM, and CPL — are held back on purpose. They publish when the first production revision is actually ordered: the commitment is that the ordered revision's full manufacturing package goes public, not that every intermediate spin does. Schematics and firmware are public now; fabrication files follow the first order.

## Status — honest version

These schematics are the v5 design baseline. The boards are in layout respins (main v7, NFC v8; IR complete) and have **not yet passed the final DRC gate** — nothing has been fabricated or ordered yet. Pin maps and interfaces are frozen; placement and routing are what the respins are finishing. Treat these as the design of record for review and learning, not as a fabrication package.
