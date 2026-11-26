# IMP-2 sweep matrix — design + evidence

Reference: IMP-2 layout pass 2. Banked best `imp2_layout.kicad_pcb` md5
`4864210b6cce60b3e11ee53a8a3a5d43`, DRC **134 unconnected / 0 shorts /
12 clearance** (total 412). Family board `imp2_placement.kicad_pcb`
md5 `3a304bde7b3646c6b08cda668a77b53d` — the pass-2 placement rebuilt from
stage2 + the verbatim `pass2_zones.py` GND-zone recipe, 0 tracks, 0 vias;
verified baseline **283/0/12, total 391** (exactly the log's placement
baseline). All counts below are from
`files/designs/imp2/layout/ROUTING_LOG.md` + the committed
`imp2_layout_drc.rpt` (re-parsed 2026-11-25: 134/0/12, total 412 —
matches the log exactly), unless noted.

## Families — interactive-pass strategies

The pass-2 log's verdict: "the remaining opens are dense-fanout work
(U1 0.5 mm castellation escape, U12 0.4 mm pitch with 0.6/0.3 mm vias)
that needs an interactive pass or a placement/fanout change, not more
autorouter passes." So the family axis is WHICH dense-fanout problem the
interactive pass attacks first. All four families share the placement
board (`imp2_placement.kicad_pcb`, baseline 283/0/12); they differ by
driver stage order (targeted stages are TODO in v1 — the wrappers run the
shared Freerouting control stage and say so). The banked 134 board is the
number to beat, never a board to route over.

| Family | Strategy | Evidence (opens) |
|---|---|---|
| `i2a` U7-first | TCA9535 expander fanout + I2C spine first | U7 13 opens (worst ref, tied); I2C_SCL 11 + I2C_SDA 4 ride this bus; U7 QFN-24 4x4 P0.5 |
| `i2b` U12-field-first | FUSB302B 0.4 mm field escape first | U12 13 opens (worst ref, tied); PD_INT_N 4; ALL 12 fenced clearance live here (U12 pads, 0.15 vs 0.2 mm rule) |
| `i2c` power-first | GND/V3_3/VBUS spine before signals | GND 18 + V3_3 18 + VBUS_5V 9 = 45/134 (34%); zone refill after routing disconnected GND pads (starved_thermal 24 -> 44 after -mp 1) |
| `i2d` case-first | Outline + mounting locked first, board routes inside it | Outline already = ruled 29318 61.00 x 92.44 mm; 9 copper_edge_clearance traces at 0.22-0.49 mm from the edge (rule 0.5 mm) |

### Case-first (i2d) — the required axis, with its honest trade

Reuse of the IMP-1 Family D (outline/case-first) pattern: lock the
enclosure/outline + mounting first, then the board must fit it and route
inside it. For IMP-2 the outline is already the 29318 module outline, so
the case-first work is the mounting holes + the 0.5 mm edge-keepout frame.
TRADE, carried from IMP-1 v17: a locked outline can FENCE the routing —
v17's fence proofs showed exactly that failure mode (fine raster grazes,
coarse raster provably cannot exit 0.5-pitch QFN fields, 0.375 available
vs 0.415 demanded). A case-first entry is only a win if the board routes
inside the locked outline. If a case-first variant can't route, it is
reported as FENCED with the proof (DRC counts + fencing nets), never
forced. ASSUMPTION CARRIED EXPLICITLY: the 29318 mounting-hole pattern
is UNVERIFIED (ROUTING_LOG fenced item 3; LAYOUT_LOG pass-2 item) — any
hole geometry this family adds is assumption-flagged, never fact.

## Variants — the real open decisions

Autorouter variants are a CONTROL/BASELINE, not the main axis (per the
log: not more autorouter). Each variant is a router configuration that
actually changes the DSN/SES flow:

| Variant | What changes | Evidence / why it is in |
|---|---|---|
| `fr-mp1` | Freerouting 1 pass, fast partial | Log: 194 unconnected (89 s + 22 s). Lower bound of the autorouter. |
| `fr-mp3` | Freerouting 3 passes (keeper config) | Log: 134 unconnected — reproduces the reference. THE control. |
| `fr-mp3-finevia` | mp3 + via dia 0.6 -> 0.5 mm (drill stays 0.3) | Targets U12 0.4 mm-pitch + U1 0.5 mm castellation escapes. DSN edit is exact: padstack `Via[0-3]_600:300_um` (182 refs) -> `Via[0-3]_500:300_um`, circles 600 -> 500; driver verifies counts before FR runs. Caution: annular ring lands exactly at the 0.1 mm pro minimum — DRC judges. |
| `fr-mp3-edge` | mp3 + 0.5 mm edge-keepout frame attempt | Targets the 9 copper_edge_clearance traces. KNOWN RISK: the pass-2 attempt proved KiCad 7 DSN export carries footprint keepouts only — board keepout zones did NOT appear in the DSN (28 keepouts before and after). The driver verifies the DSN keepout count before/after and records a no-op if unchanged; a no-op runs as plain mp3 and the result proves it. |

4 families x 4 variants = **16 jobs**.

## Axes dropped (with the evidence)

- `fr-mp10`: killed at 21 min, still auto-routing, no output; passes after
  the first are slow rip-up/retry (~90 s+ each) and the temple precedent
  says extra passes can regress.
- `astar-*` (0.02/0.05/0.09): no blib A* driver exists for the IMP-2
  netlist. IMP-1's A* is `tools/sweep/v18a/` (`blib.py`, `route17.py`) —
  porting it to IMP-2's 150 nets / 548 nodes is its own work item. The
  harness accepts the names; jobs report `blocked` until a driver lands.
- Net-priority orders as a standalone axis: Freerouting takes no net
  priority input — priority only exists as stage order inside a family
  driver (route X first, then the rest), which is the family axis, not a
  variant. Listing it as a variant would be fiction.
- Placement-respin families (J3/U13/U2 fields): the log's honest next step
  is a placement respin, but no respin placement script exists. v2 axis.
- U16 CHIPEN fix: schematic/symbol fix (correct symbol to the 6-land
  INMP441 pinout or verify CHIPEN vs the TDK datasheet) before the mic is
  populated. Contributes 0 to the 134 count — no U16 pad-7 line exists in
  the DRC report. Gate, not an axis.
- J3 pin-order verification: ASSUMED-VERIFY hard gate before fab (verify
  by continuity check on the physical 29318 module or the wiki table).
  Moves 0 in the DRC count. Gate, not an axis.
- Mounting holes as a standalone axis: pattern unverified; holes are part
  of the i2d family, not their own axis.

## Gates that stay OUT of the sweep (hard, before any fab)

1. J3 pin order ASSUMED-VERIFY (de-facto 3.5in SPI+CTP 14-pin order
   extended to 15; MISO new; pin 11 shares DISP_RST; pin 15 NC).
2. U16 pin 7 (CHIPEN) has no land — symbol/footprint mismatch; fix before
   populate. U16/U17 are PHASE2 DNP.
3. Mounting holes not added — 29318 hole pattern unverified.
4. Pin numbering ASSUMED-VERIFY on U4/U7/U12/U14 (datasheet pass).
5. ANT2 coil turns not drawn (tuning-pass item); TPS2553 reverse-blocking
   in device mode unconfirmed; I2S_BCLK phase-2 sourcing open.
6. Finish line: DRC-clean (0/0/0). Nothing here claims fab-ready.

## Verdicts

- `beat`: unconnected < 134 AND shorts == 0 AND clearance <= 12.
- `tie`: unconnected == 134 AND shorts == 0 AND clearance <= 12.
- `no-beat`: anything else, including any clearance growth above 12.
- A `beat` is progress toward 0/0/0, not a pass. Gate PASS still requires
  Trevor + evidence.
