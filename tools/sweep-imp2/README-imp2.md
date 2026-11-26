# IMP-2 routing sweep — harness

Parallel sandbox matrix on GitHub Actions (`.github/workflows/sweep-imp2.yml`),
adapted from the proven IMP-1 sweep (`tools/sweep/`, repo commit ec33efce).
Same repo (`deadbuglabs/imp-1`), same `sweep-results` branch, results
namespaced under `results/imp2/` so they never collide with IMP-1's.

## Reference — two boards, like IMP-1 (v15 banked vs v18a placements)

- Banked reference (verdicts + toolchain smoke ONLY):
  `hardware/pcb/imp2/imp2_layout.kicad_pcb` — IMP-2 layout pass 2,
  Freerouting `-mp 3` keeper: **134 unconnected / 0 shorts / 12 clearance**
  (md5 `4864210b6cce60b3e11ee53a8a3a5d43`). NEVER routed over: routing it
  again stacks a second route on the first (proven locally 2026-11-25:
  FR mp1 on it gave 132 with 179 `holes_co_located` duplicate vias).
- Family board (what sweep jobs actually route):
  `hardware/pcb/imp2/imp2_placement.kicad_pcb` — the pass-2 placement
  rebuilt from `imp2_layout_stage2.kicad_pcb` + the verbatim
  `pass2_zones.py` GND-zone recipe (3 zones, 0.25 mm clearance).
  0 tracks, 0 vias, 143 footprints (md5
  `3a304bde7b3646c6b08cda668a77b53d`). Verified baseline **283 unconnected
  / 0 shorts / 12 clearance**, total 391 — exactly the log's placement
  baseline. All families share it in v1; families differ by driver.
- The `.kicad_pro` sibling must stay name-matched for each board, or KiCad
  falls back to default project settings and the counts inflate. The 12
  clearance are FENCED: all 12 cite U12 pads (0.4 mm-pitch pads 0.15 mm
  apart vs the 0.2 mm Default netclass) — footprint-vs-rule mismatch,
  within standard 4-layer fab capability. Total DRC violations on the
  banked board: 412 (incl. 9 copper_edge_clearance, 36 starved_thermal,
  12 drill_out_of_range, 1 hole_clearance, 108 silk_overlap, 81
  silk_over_copper, 10 silk_edge_clearance, 143 lib_footprint_issues —
  bookkeeping/cosmetic classes, not the headline).
- The board is NOT DRC-clean and NOT fab-ready. Finish line: DRC-clean
  (0/0/0). Beating the 134 reference is progress toward it, not a pass.
- Local validation (2026-11-25, KiCad 7.0.11, before the push): DRC on the
  staged banked board reproduced 134/0/12 EXACTLY, per-type counts
  identical to `imp2_layout_drc.rpt`; DRC on the rebuilt placement
  reproduced the log's placement baseline 283/0/12 EXACTLY (total 391).
  Full driver loop proven on the placement board (DSN export -> FR mp1 ->
  SES import -> DRC; see `validation/VALIDATION.md`).
  `check_imp2.py` 70/70 PASS on the schematic netlist.

## What changed vs the IMP-1 harness (and why — all deliberate)

Shared core copied verbatim (identical files): `drc.py`, `parse_drc.py`,
`export_dsn.py`, `import_ses.py`. IMP-2-specific, parameterized:
- `run_variant_imp2.sh` — family board is the shared placement
  (`hardware/pcb/imp2/imp2_placement.kicad_pcb`); families differ by
  DRIVER (interactive-pass strategy), not by board. The banked 134 board
  is NEVER routed over (see Reference). Placement gate allows clearance
  == 12 (fenced) and blocks only growth above 12 — IMP-1's gate
  (clearance > 0 blocks) would block every IMP-2 job before routing.
  The family board's baseline is not checked against 134 (it is a
  placement: v1 baseline 283/0/12); the toolchain job checks the banked
  134 board instead.
- `write_results_imp2.py` — schema `imp2-sweep-result/1`, reference
  134/0/12. `beat` = unconnected < 134 AND shorts == 0 AND clearance <= 12.
  Clearance growth above 12 is always `no-beat`.
- `check_failfast_imp2.py` — same rules, threshold 134 instead of 15.
- `push-results-imp2.sh` — paths `results/imp2/<run>/<family>-<variant>.json`.
- `import_ses_imp2.py` — parameterized copy of the SES importer proven for
  this board (direct SES parse; stock `pcbnew.ImportSpecctraSES` needs the
  GUI board, which is None standalone). Via dia/drill parsed from the SES
  padstack name, so the finevia variant's 0.5 mm vias survive import.
- `fr/run.sh` — shared Freerouting driver used by all four family wrappers.
  No snapshot watcher: the v1 matrix is Freerouting-only (see below).

## Matrix (see MATRIX.md for the evidence behind each axis)

Families (interactive-pass strategies): `i2a` (U7-first), `i2b`
(U12-field-first), `i2c` (power-first), `i2d` (case-first).
Variants: `fr-mp1` (fast partial), `fr-mp3` (keeper control),
`fr-mp3-finevia` (0.5 mm vias for the 0.4/0.5 mm-pitch escapes),
`fr-mp3-edge` (0.5 mm edge-keepout frame probe).
4 families x 4 variants = 16 jobs. `fail-fast: false` on the matrix;
fail-fast lives per job. Each sweep job capped at 90 min.

V1 HONESTY NOTE: the family-targeted stages (hand escape / power spine /
outline work) are NOT yet implemented — the family wrappers run the shared
Freerouting control stage only and record that in every results JSON
(`driver_notes.txt` appended to `reason`). The first dispatch therefore
establishes the control baseline per family slot; targeted stages land as
family-driver upgrades. A job whose family driver is absent reports
`blocked` and burns no router time.

## Excluded axes (with evidence — do not re-add without new evidence)

- `fr-mp10`: killed at 21 min with no output in the pass-2 log; passes
  after the first are slow rip-up/retry and the temple precedent says
  extra passes can regress.
- `astar-*`: no blib A* driver exists for the IMP-2 netlist. The harness
  supports the names, but until a driver is ported the jobs report
  `blocked`. Porting the A* driver is future work.
- Placement-respin families: the log's honest next step is a placement
  respin around J3/U13/U2, but no respin placement script exists yet.
  That is a v2 matrix axis, not v1.
- U16 CHIPEN fix, J3 pin-order verification, mounting holes: gates, not
  sweep axes. U16 pin 7 has no land and contributes 0 to the 134 count
  (no U16 pad 7 line in the DRC report); J3 pin order is ASSUMED-VERIFY
  (verify by continuity check on the physical module before any fab);
  the 29318 hole pattern is unverified. None of these moves the DRC
  count, so none belongs in the matrix.

## Fail-fast (per job)

1. Placement gate: baseline DRC before routing. Must be exactly 134/0/12;
   shorts > 0 or clearance > 12 -> status `blocked`, no routing attempted.
2. Stage gate: drivers call `check_failfast_imp2.py <prev> <cur>` after each
   routing stage. STOP when a stage closed nothing while still at >= 134
   unconnected, when shorts appear, or when clearance grows.
3. Job wall clock: 90 min (`timeout-minutes`) so a hung router dies.

## Family driver interface

A family driver lives at `tools/sweep-imp2/<family>/run.sh` (executable),
called as `run.sh <variant> <board.kicad_pcb> <workdir>`. It routes the
board in place and may write stage DRCs with `drc.py` + `parse_drc.py`.
It must call `check_failfast_imp2.py` after each stage and stop when it
prints STOP. The harness runs the final DRC and writes the results JSON;
the driver must not. Drivers must not assume workspace paths, locks, or
sibling workers — each job is its own runner. No absolute `/home/...` paths.

## Live results

Every job pushes its results JSON to branch `sweep-results` as soon as it
finishes (see `push-results-imp2.sh` — detached worktree, rebase retry):
- Per job (live): `results/imp2/<run-number>/<family>-<variant>.json`
- Toolchain smoke: `results/imp2/<run-number>/toolchain-imp2-baseline.json`
- Combined summary (written by the `aggregate` job after the matrix):
  `results/imp2/summary.json` and `results/imp2/history/<run-number>-summary.json`

Raw reads (no auth needed, public repo):
```
https://raw.githubusercontent.com/deadbuglabs/imp-1/sweep-results/results/imp2/summary.json
https://raw.githubusercontent.com/deadbuglabs/imp-1/sweep-results/results/imp2/<run-number>/<family>-<variant>.json
```

Result JSON schema `imp2-sweep-result/1`: `board` (`imp2`), `family`,
`variant`, `status` (`done` | `blocked` | `failfast` | `toolchain-ok`),
`reason`, `reference` (134/0/12), `verdict_vs_reference`
(`beat` | `tie` | `no-beat` | `n/a`), `drc`, `baseline_drc`,
`board_md5`, `finished_utc`.

Each job also uploads its board + DRC report + logs as an artifact
(`result-imp2-<family>-<variant>`, 30 days) and writes a job summary.

## Live snapshots

NONE in v1 — stated plainly, not faked. The A* snapshot contract
(cumulative plan + progress.jsonl + stage drc.json, watched by
`snap_watch.py`) needs a driver that checkpoints a routing plan after
every link. Freerouting is a black box: it writes its `.ses` only at the
end, so there are no mid-route checkpoints to watch. IMP-2's routing path
is Freerouting-only in v1, so there is no live snapshot feed — live
results JSON + the final-board replay in the job artifact are the whole
live surface. If an A* driver is ever ported to IMP-2, a watcher port
(imp2 snapshot schema + `results/imp2/` paths) lands with it; do not reuse
the IMP-1 watcher (schema `imp1-sweep-snap/1`, `results/<run>/` paths)
unadapted.

## Triggers

- `workflow_dispatch` (Actions tab): full matrix. Inputs narrow families /
  variants if needed.
- Push to `.github/workflows/sweep-imp2.yml`: toolchain smoke job only.
- Schedule `0 */4 * * *`: full matrix every 4 hours.
