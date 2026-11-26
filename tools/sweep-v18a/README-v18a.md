# v18a cell driver — GitHub Actions port

Ports the local v18a sweep supervisor (`hidden_files/v18a-sweep_work/`,
`supervise.sh` + `supervise.py`) to one cell per Actions job, so a cell can
run past the ~1.5–1.9 h VM lifetime that killed every local S01/S04 attempt.
Authored as files only, staged under `tools/sweep-v18a/` mirroring repo
layout. Nothing here pushes to GitHub by itself; the workflow calls
`run_cell.sh` then `push-results-v18a.sh`.

Family board: `hardware/pcb/main-v5/imp1_main_v18a.kicad_pcb` — the ripped
placement, baseline **146/0/0** (unconnected/shorts/clearance). Target: beat
v15 **15/0/0**. Gate 3 NOT PASS unless a cell is genuinely 0/0/0. Never touch
frozen v15/v17 files. Cells work on copies in `$WORKDIR` only.

Common constants (all cells): A* widths power `0.4,0.25`, else `0.25`;
PITCH 0.05, CLR 0.20, via 0.6/0.3. A* exclusions (floor 5 entries): ROW3
(frozen), CHG_PGOOD + ENC_PUSH (accepted bodges). FR cells route the whole
DSN netlist; the engine decides, outcome recorded as-is.

## Files

| File | What it is |
|---|---|
| `run_cell.sh` | The core driver. `run_cell.sh <cell> <board.kicad_pcb> <workdir>` |
| `rebuild_state.py` | Rebuild a checkpoint board from state (pure replay, no routing) |
| `import_ses.py` | Port of the local manual-parse SES import (repo-relative `sys.path`) |
| `inject_autoroute.py` | Copy of local, unchanged except header note |
| `stitch17.py` | Copy of local, unchanged except header note + repo-relative `sys.path` |
| `phase_cmds.txt` | Local file verbatim (36 lines, byte-identical) + port addendum |
| `push-results-v18a.sh` | Push result + state + artifacts to `sweep-results` |
| `README-v18a.md` | This file |

Reused in place from the repo (NOT copied): `tools/sweep/write_results.py`,
`tools/sweep/push-results.sh` (pattern), `tools/sweep/check_failfast.py`,
`tools/sweep/parse_drc.py`, `tools/sweep/drc.py`, `tools/sweep/export_dsn.py`,
`tools/sweep/v18a/route17.py`, `tools/sweep/v18a/blib.py`,
`tools/sweep/v18a/apply_plan.py`. KiCad 7.0.11 has no `kicad-cli pcb drc` —
DRC is pcbnew `WriteDRCReport` via `tools/sweep/drc.py` under `xvfb-run`.

## Cell table

| Cell | Driver | Order / notes |
|---|---|---|
| S01 | FR default | Chunk loop, `-mp 1`, cap 60, until done/stall |
| S02 | FR via-minimal | Chunk loop + DSN inject `via_costs=500, plane_via_costs=50`, cap 60 |
| S03 | FR rip-up-aggressive | Chunk loop + DSN inject `start_ripup_costs=10`, cap **100** |
| S04 | A* MF 0.02 cage/power-first | A, B, C, D |
| S05 | A* MF 0.02 signal-first | C, B, A, D |
| S06 | A* MF 0.05 cage/power-first | A, B, C, D |
| S07 | A* MF 0.05 signal-first | C, B, A, D |
| S08 | A* MF 0.09 cage/power-first | A, B, C, D |
| S09 | A* MF 0.09 signal-first | C, B, A, D |
| S10 | A* hybrid, cage/power-first | **E-exits (0.02)**, A, B, C, D (0.09) |
| S11 | A* hybrid, signal-first | **E-exits (0.02)**, C, B, A, D (0.09) |
| S12 | FR bulk + A* 0.02 finish + stitch | FR chunk loop, then A/B/C/D-finish at 0.02 power-first, then stitch |
| S13 | Pour-first + A* 0.05 + stitch | D-pours-first, A, B, C (0.05), then stitch; handvia pre-step unwired |

### Exact commands

S01, every FR chunk (chunk `N`, workdir `$WORK`):

```bash
timeout 2400 xvfb-run -a java -jar "$REPO_ROOT/tools/freerouting/freerouting.jar" \
  -de "$WORK/chunk_N.dsn" -do "$WORK/chunk_N.ses" -mp 1
```

S04, phase 1 (A-power, MF 0.02):

```bash
timeout 3000 xvfb-run -a python3 tools/sweep/v18a/route17.py "$WORK/board.kicad_pcb" \
  "$WORK/plan_A-power.json" "$WORK/log_A-power.txt" 0.02 \
  BOOST_SW:0.4,0.25 SYS_5V:0.4,0.25 BAT_CELL:0.4,0.25 CELL_POS:0.4,0.25 \
  CELL_NEG:0.4,0.25 BOOST_5V:0.4,0.25 BOOST_FB:0.4,0.25 A9_PWR:0.4,0.25 \
  A9_GATE:0.4,0.25 A9_5V:0.4,0.25 VBUS_5V:0.4,0.25
```

Each A* phase is followed by: validate plan JSON (`python3 -m json.tool`),
`apply_plan.py`, DRC (`drc.py` + `parse_drc.py`), and the
`check_failfast.py` gate against the previous counts (STOP ends the cell as
`failfast`).

## Decisions recorded once (the port's rulings)

### 1. Chunking supersedes the single-run `-mp` specs

The CELL_LOG specs say S01/S02 run with no `-mp` flag and S03 with
`-mp 100`. The chunked driver runs **every FR chunk at `-mp 1`** — the
sibling supervisor forces `-mp 1` per chunk, and a single `-mp 100` run
cannot resume across jobs. S03's "-mp 100" becomes a **100-chunk cap**;
S01/S02 run until done/stall (cap 60). One decision, recorded here and in
`run_cell.sh`'s header.

### 2. S10/S11 "0.02 QFN exits" — the port's definition

No runnable definition existed on disk: `phase_cmds.txt` shows all S10/S11
phases at MF 0.09, identical to S08/S09 except order, and
`grep qfn|exit|fanout` over the work dir returns 0 hits. Port definition:
**QFN exits = the B-cage net set** — the 17 RF/CC/SPI nets exiting the dense
QFN fields: `RF_P RF_N BAL_A BAL_B BAL_C BAL_D BAL_E BAL_BDC RF_ANT
CC_DCOUPL CC_XOSC_Q1 CC_XOSC_Q2 CC_RBIAS SPI_SCLK SPI_MOSI SPI_MISO CC_CS`.
S10/S11 run an extra exits-first phase at MF 0.02 over that set, then the
bulk phases at MF 0.09 in their normal order (S10: exits, A, B, C, D; S11:
exits, C, B, A, D). The phase lines live in the `phase_cmds.txt` port
addendum as `S10 E-exits 0.02 ...` / `S11 E-exits 0.02 ...`. This is the
port's definition, not a recovered spec — if Trevor rules otherwise, the
addendum lines change and the state files' `done_phases` for S10/S11 are
void for the exits phase.

### 3. S12 A* 0.02 finish — constructed, no spec on disk

After the FR bulk loop ends (done/stall/cap/time), S12 runs A* 0.02 finish
phases in power-first order (`A-finish, B-finish, C-finish, D-finish` at
MF 0.02, same net lists as S04 phases A–D — see the `phase_cmds.txt`
addendum), then `stitch17.py` on the DRC report + apply + final DRC.
The `-finish` suffix keeps these phases distinct from any other cell's.

### 4. S13 handvia pre-step — unwired, not faked

`handvia.py` needs explicit `NET:REF.PIN` targets and none are defined on
disk. S13 runs the pour-first phase order already in `phase_cmds.txt`
(D-pours-first, A, B, C at MF 0.05) + stitch finish. The handvia pre-step
is recorded as unwired/manual follow-up. No targets were invented.

## Resume design

A cell's progress survives a dead runner because **the board is never
committed** — it is rebuilt from pristine + sessions/plans:

Committed after each chunk/phase (pushed at job end by
`push-results-v18a.sh`):
- `.ses` files — one per completed FR chunk, `results/v18a/<CELL>/ses/chunk_NNN.ses`
- plan JSONs — one per completed A* phase, `results/v18a/<CELL>/plans/plan_<PHASE>.json`
- state JSON — `results/v18a/<CELL>-state.json`, schema
  `imp1-sweep-v18a-state/1`: `cell, chunk, stall, done_phases[],
  last_drc{}, ses_files[], plan_files[], updated_utc, run_number`

NOT committed: the board. The next run rebuilds it with
`rebuild_state.py <state.json> <pristine-board> <out-board> [artifacts-root]`,
which replays `ses_files` in order (via `import_ses.py`) then `plan_files`
in order (via `apply_plan.py`) onto a fresh copy of the pristine board.

**Rebuild verification — this IS the resume proof.** After rebuilding,
`run_cell.sh` runs a full DRC on the rebuilt board and requires
`unconnected == state.last_drc.unconnected`. Mismatch → the cell is
reported `blocked`, reason `rebuild-mismatch`, and no routing runs on the
unverified board. A matching count is the only trust the resume gets.

Mid-run caveat, stated honestly: state and artifacts are mirrored into the
`sweep-results` worktree on every chunk/phase, but only **pushed at job
end**. A runner killed mid-job loses the chunks since the last push; the
next run resumes from the last pushed state. That is the accepted cost of
not pushing (and racing) on every chunk.

Results: `results/v18a/<run>/<CELL>.json` on the `sweep-results` branch,
schema `imp1-sweep-result/1`, `family="v18a"`, `variant=<CELL>`, plus one
extra field `cell_state` (`{chunks, stall, done_phases, resume_from,
checkpoint_ref}`) injected after `write_results.py` runs. `board_md5` is
the working board's md5 at result time.

## Ownership rule

**A cell owned by Actions is NOT run by the local supervisor, and vice
versa.** The local supervisor (`supervise.sh`, cron-driven) is the fallback
for cells Actions has not taken. Ownership is decided per cell before a run
starts and recorded in the cell's state file by whichever side takes it —
the first side to write state for a cell owns it until that cell is done or
explicitly handed over. Two drivers routing the same cell would diverge the
board; the state file is the single claim token. If both sides ever hold
state for one cell, the Actions state wins only if its `updated_utc` is
newer AND the local supervisor has not advanced the cell since — otherwise
stop and ask Trevor.

## Timeouts and budgets (300-min job cap)

| Budget | Value | Why |
|---|---|---|
| FR chunk (`timeout`) | 2400 s (40 min) | `supervise.sh` value; well under VM lifetime |
| A* phase (`timeout`) | 3000 s (50 min) | `run_astar_phase.sh` value; plan checkpoints per link |
| DRC (`timeout`) | 900 s | DRC is minutes; 900 s is a failure, not a wait |
| Stop starting new work | 240 min elapsed | Leaves room for final DRC + push inside the 300-min job |
| FR chunk cap | 60 (S03: 100) | S03's `-mp 100` reinterpreted as a chunk cap |
| FR stall limit | 3 consecutive chunks with unconnected not improving | Same stall rule as the local supervisor |
| FR start watchdog | 300 s to see "Starting auto-routing" | A rejected input hangs FR on a dialog (`supervise.sh` lesson) |
| FR failure retry | ONE retry from a freshly re-exported DSN | Board-mediated retry, mirroring `supervise.sh` |

Exit contract: `run_cell.sh` exits **0 whenever a results JSON was
produced** — `blocked`, `no-beat`, and `failfast` are results, not failures.
Non-zero ONLY if no result JSON could be produced at all.

## Honest status of the unproven items

- **Freerouting `-de board.dsn+chunk.ses`: UNTESTED.** Feeding a `.ses`
  back as `-de <ses>` is DISPROVEN (FR 1.9.0's `-de` reader demands a DSN;
  log: "the input file is not in a Specctra DSN file format"). The
  `dsn+ses` combined form was never tried anywhere. This driver does not
  use it — it uses board-mediated resume (import `.ses` into the board,
  re-export DSN, run FR again).
- **Board-mediated resume: DESIGNED, rebuild-verified by the smoke run,
  not yet proven end-to-end.** The import-into-fresh-pristine-copy step,
  the DSN re-export, and the rebuild-verification DRC are all implemented
  here, and the local `experiment/fr-resume/` run showed the import path
  working chunk-to-chunk. No full cell has yet completed through this
  driver in Actions; the first S01 run is the proof run. If rebuild
  verification ever fails, the cell reports `blocked/rebuild-mismatch`
  instead of routing on a suspect board.
- **`pcbnew.ImportSpecctraSES`: UNPROVEN in Actions, not used.** The repo's
  `tools/sweep/import_ses.py` uses it; it needs the GUI's current board,
  which is `None` headless. This port uses the local manual-parse
  `import_ses.py` instead (same code as the local supervisor's).
- **S10/S11 exits, S12 finish, S13 handvia:** port constructions, not
  recovered specs — see Decisions 2–4 above. Each is flagged in
  `phase_cmds.txt`'s addendum and in the per-cell result's `cell_state`.
- **FR chunk watchdog:** the 300 s "Starting auto-routing" check is copied
  from `supervise.sh`, where a rejected input hung FR on a dialog. It has
  not been observed firing in Actions yet.
