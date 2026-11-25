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

### A* driver snapshot contract (live feed)

For `astar-*` variants the harness starts `snap_watch.py` (below) and
exports `SWEEP_SNAP_DIR=$WORKDIR/snaps` and
`SWEEP_PLAN_PATH=$WORKDIR/plan.json`. The driver feeds the watcher with
files in `$WORKDIR` (all optional — a driver that writes none of them
still routes; the watcher just emits geometry-only snapshots):

- **Plan** (`$SWEEP_PLAN_PATH`, default `$WORKDIR/plan.json`): the
  cumulative routing plan, same schema as route17.py's plan —
  `add_tracks` [{`net`, `layer` (`F_Cu`/`In1_Cu`/`In2_Cu`/`B_Cu`),
  `pts` [[x, y], ...] mm, `w`}], `add_vias` [{`net`, `at` [x, y],
  `dia`, `drill`}], `delete` / `delete_vias`, `codes` {net: code} —
  checkpointed after **every link** (atomic replace preferred; the
  watcher skips a torn read and retries next poll).
- **Progress** (`$WORKDIR/progress.jsonl`, JSONL, one line per finished
  net): optional first line `{"event": "start", "nets_total": N}`, then
  `{"net": NAME, "unified": true|false, "links": n}` as each net finishes.
  Last line wins per net.
- **Stage DRC** (`$WORKDIR/drc.json`, parse_drc.py output): if the driver
  publishes its stage DRC counts here, the watcher forwards the freshest
  file verbatim into snapshots as `drc` with `drc_source: "driver-stage"`.
  This is the only way snapshots carry real DRC counts mid-run — the
  watcher itself never runs DRC (apply + zone fill + DRC per batch would
  cost more than the routing it watches).

Freerouting drivers write none of this and never see the watcher.

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

## Live snapshots (A* only — watch the traces land)

`snap_watch.py` runs beside the A* driver (started by `run_variant.sh`,
`astar-*` variants only) and turns the driver's checkpointed plan into
delta snapshots, pushed to `sweep-results` while the job is still running.
Freerouting variants never run it — the engine is a black box, so their
visual is the finished-board replay only.

- **Cadence:** one snapshot per plan growth event (typically one routed
  link); pushes batch every **10 snapshots or 30s**, whichever comes
  first, plus a final flush when the driver exits. Git pushes are the
  expensive part, so they batch — snapshots themselves are cheap local
  files. Poll interval 5s. A push takes ~2–5s against a batch of routing
  that takes 30–100s, so the overhead stays under ~10%.
- **Paths** (branch `sweep-results`):
  `results/<run-number>/<family>-<variant>/snaps/snap-NNNN.json` and
  `results/<run-number>/<family>-<variant>/snaps/index.json`.
  Raw reads (no auth needed):
  ```
  https://raw.githubusercontent.com/deadbuglabs/imp-1/sweep-results/results/<run-number>/<family>-<variant>/snaps/index.json
  https://raw.githubusercontent.com/deadbuglabs/imp-1/sweep-results/results/<run-number>/<family>-<variant>/snaps/snap-0001.json
  ```
  The viewer polls `index.json` (latest_seq, snap_count) and fetches only
  snapshots newer than the last one it drew.
- **Snapshot schema `imp1-sweep-snap/1`:** `seq`, `t_utc`, `net` (net just
  worked on), `net_unified` (true/false/null), `links_landed` (from
  progress.jsonl, null if the driver writes none), `tracks_new` /
  `vias_new` / `delete_new` / `delete_vias_new` (delta geometry since the
  previous snapshot — same entry shape as the plan), `plan_tracks` /
  `plan_vias` (cumulative counts), `progress` (`nets_unified`,
  `nets_sealed`, `nets_remaining`, `nets_total` — nulls when the driver
  writes no progress file), `unconnected_estimate` (nets not yet unified;
  the router's own count, NOT a DRC count), `drc` + `drc_source`
  (`driver-stage` when the driver published stage DRCs, else null).
  The viewer accumulates `tracks_new`/`vias_new` and applies
  `delete_new` to draw the traces laid so far.
- **Index schema `imp1-sweep-snap-index/1`:** `latest_seq`, `pushed_seq`,
  `snap_count`, `snaps` (filenames), `links_landed`, `progress`,
  `unconnected_estimate`, `drc`, `drc_source`, `updated_utc`.
- **Honesty rules:** snapshot counts are router progress unless `drc`
  is present. The authoritative DRC is the harness's final DRC in the
  results JSON. A job whose driver writes no plan produces no snapshots
  (index.json still appears, `snap_count: 0`).
- **Local files:** the job's `out/snaps/` directory holds the same files
  and is included in the uploaded artifact. `push-snaps.sh` pushes them
  (`push-snaps.sh <snapdir> <run_number> <family> <variant>`); it keeps a
  persistent worktree at `/tmp/sweep-snaps-wt`, distinct from
  `push-results.sh`'s worktree, and retries fetch+rebase+push 5 times.

## Triggers

- `workflow_dispatch` (Actions tab): full matrix. Inputs narrow families
  / variants if needed.
- Push to `.github/workflows/sweep.yml`: toolchain smoke job only.
