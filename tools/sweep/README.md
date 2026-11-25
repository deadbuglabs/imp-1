# IMP-1 placement sweep — harness

Parallel sandbox matrix on GitHub Actions (`.github/workflows/sweep.yml`).
Each family × variant runs as its own job on `ubuntu-24.04` with KiCad 7,
Java 21, Xvfb and Freerouting 1.9.0 (jar downloaded at runtime from the
verified release — no binaries committed).

## Reference

- Source board: `hardware/pcb/main-v5/imp1_main_v15.kicad_pcb` — v15, the best
  banked board: **15 unconnected / 0 shorts / 0 clearance** (md5
  `80ca73516db8dbfb689ed46fae7b35cc`). The `.kicad_pro` sibling must stay
  name-matched, or KiCad falls back to default project settings and the
  counts inflate.
- Gate 3 is NOT PASS. A family "beats" v15 only with <15 unconnected at
  0 shorts / 0 clearance.

## Matrix (order = most promising first)

Families: `v18a` (QFN-field-first — furthest along, placement verified
0 collisions), `v18b` (power-tree-first — targets the 7-net V3_3 cage
cluster), `v18c` (free choice), `v18d` (outline/case-first).

Variants: `astar-0.05` (blib A* at its native 0.05mm pitch, proven across
v9–v18), `freerouting-mp3` (Freerouting 1.9.0, 3 passes — the middle of the
imp2 runs), `astar-0.02` (fine raster — v17 evidence says it grazes, but
QFN exits need it), `astar-0.09` (coarse raster — the v17 fence proof says
0.375mm available vs 0.415mm demanded on v17's placement, so it cannot
exit v17's QFN fields; the v18 placements change the exit geometry, so the
proof does not transfer and the variant stays in), `freerouting-mp1`
(fast, partial), `freerouting-mp10` (slowest, most thorough).

4 families x 6 variants = 24 jobs, all at once up to the 20-job public
concurrency (the last 4 queue behind the first 20). `fail-fast: false`
on the matrix so one family's failure never cancels the rest — fail-fast
lives per job, below. Toolchain installs are cached between runs (apt
archives + Freerouting jar, `actions/cache`); each sweep job is capped at
90 min so a hung router dies.

## Fail-fast (per job)

1. Placement gate: baseline DRC before routing. Shorts > 0 or clearance
   > 0 -> status `blocked`, no routing attempted.
2. Stage gate: family drivers call `check_failfast.py <prev_counts.json>
   <cur_counts.json>` after each routing stage. It prints STOP when a stage
   closed nothing while still at >= 15 unconnected, when shorts appear, or
   when clearance grows. The driver stops there and reports.
3. Job wall clock: 50 min (`timeout-minutes`) so a hung router dies.

## Family driver interface

A family lands its driver at `tools/sweep/<family>/run.sh` (executable),
called as:

```
run.sh <variant> <board.kicad_pcb> <workdir>
```

- `<variant>` is one of `astar-0.02`, `astar-0.05`, `astar-0.09`,
  `freerouting-mp1`, `freerouting-mp3`, `freerouting-mp10`. For
  `freerouting-*`, the pass count is the number after `-mp`; use
  `export_dsn.py` / `import_ses.py` in this directory and the jar at
  `tools/freerouting/freerouting.jar` (downloaded by the workflow).
  For `astar-*`, the raster is the number after the dash (mm).
- The driver routes the board **in place** and may write stage DRCs with
  `drc.py` + `parse_drc.py`. It must call `check_failfast.py` after each
  stage and stop when it prints STOP.
- The harness runs the final DRC and writes the results JSON; the driver
  must not.
- Drivers must not assume workspace paths, locks, or sibling workers —
  each job is its own runner. No absolute `/home/...` paths.

Until a family's board (`hardware/pcb/main-v5/imp1_main_<family>.kicad_pcb`
+ `.kicad_pro`) and driver are both in the repo, its jobs report
`blocked` with the exact reason and burn no router time.

## Live results (watcher paths)

Every job pushes its results JSON to branch `sweep-results` as soon as it
finishes (see `push-results.sh` — detached worktree, rebase retry):

- Per job (live): `results/<run-number>/<family>-<variant>.json`
  e.g. `results/12/v18a-astar-0.05.json`
- Toolchain smoke: `results/<run-number>/toolchain-v15-baseline.json`
- Combined summary (written by the `aggregate` job after the matrix):
  `results/summary.json` and `results/history/<run-number>-summary.json`

Raw reads (no auth needed, public repo):

```
https://raw.githubusercontent.com/deadbuglabs/imp-1/sweep-results/results/summary.json
https://raw.githubusercontent.com/deadbuglabs/imp-1/sweep-results/results/<run-number>/<family>-<variant>.json
```

Result JSON schema `imp1-sweep-result/1`: `family`, `variant`, `status`
(`done` | `blocked` | `failfast` | `toolchain-ok`), `reason`,
`drc` (`unconnected`, `clearance`, `shorts`, `total`, per-type counts),
`baseline_drc`, `verdict_vs_v15` (`beat` | `tie` | `no-beat` | `n/a`),
`board_md5`, `finished_utc`.

Each job also uploads its board + DRC report + logs as an artifact
(`result-<family>-<variant>`, 30 days) and writes a job summary with the
DRC numbers.

## Triggers

- `workflow_dispatch` (Actions tab): full matrix. Inputs narrow families
  / variants if needed.
- Push to `.github/workflows/sweep.yml`: toolchain smoke job only.
