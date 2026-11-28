#!/bin/bash
# v18b family driver — IMP-1 Main placement sweep (GitHub Actions).
# Contract (tools/sweep/README.md, "Family driver interface" + "A* driver
# snapshot contract (live feed)"):
#   run.sh <variant> <board.kicad_pcb> <workdir>
# Variants: astar-0.02, astar-0.05, astar-0.09,
#           freerouting-mp1, freerouting-mp3, freerouting-mp10.
#
# *** CLEARANCE BASELINE IS 26, NOT 0 — READ THIS BEFORE TOUCHING THE GATES ***
# v18b is a routed board at 31 unconnected / 0 shorts / 26 clearance
# (items_not_allowed 31; published 2026-11-26, FROZEN, Gate 3 NOT PASS;
# v15 15/0/0 remains best). The 26 clearance are pre-existing placement /
# MF-0.02 grazes (log §7: all 26 report 0.2000, i.e. copper exactly at the
# minimum — the 0.05 mm raster's quantization in the 0.5 mm-pitch QFN field).
# The driver contract's placement gate ("shorts > 0 or clearance > 0 ->
# blocked") would block v18b permanently, so EVERY gate in this driver is
# GROWTH-vs-baseline, never vs zero:
#   - stage_damaged() below: a stage is damaged only if clearance or shorts
#     GREW vs the previous stage's counts (baseline 31/0/26).
#   - check_failfast.py (repo tool) already compares clearance vs the
#     previous stage, not vs zero — no change needed there.
#   - The PREV fallback when no baseline counts file exists is the frozen
#     baseline 31/0/26, NOT 0 clearance (the v18d fallback of 0 would make
#     the first stage STOP on the baseline's own 26).
#   - The board a job finishes with is therefore never worse than 31/0/26
#     on clearance/shorts.
# HARNESS NOTE (for the harness owner, not this driver): run_variant.sh's
# placement gate as written blocks any baseline with clearance != 0, so
# v18b jobs report `blocked` before this driver ever runs. That gate needs
# a family-baseline allowance (v18b: clearance <= 26, shorts == 0) before
# the v18b matrix cells can route. This driver cannot fix that from inside
# the job — it only guarantees growth-vs-baseline behaviour once it runs.
# .PRO NOTE: the staged imp1_main_v18b.kicad_pro is v18b's OWN project file
# (md5 0caf8d78ffc0ec7bc1e36f3a1a5bf22f), NOT the v15/v18a/v18c/v18d pattern
# (cca4661e24eb2c34ab2b69e70d9c08fb). The two differ in min_copper_edge_
# clearance (0.5 vs 0.2) and min_through_hole_diameter (0.3 vs 0.2), and
# only the own .pro reproduces the frozen published DRC (31/0/26, total
# 719, INA 31) — the pattern .pro gives 30/0/26, total 578. The frozen
# verdict is the authority, so the own .pro ships.
#
# v18b is the power-tree-first family: the power tree is MIRRORED into the
# north band, U9 CC1101 stays at v15 (44,9), outline frozen at v15 (log §1).
# The family run closed 78 -> 31 unconnected (B3/B3b unified RF_P,
# CC_XOSC_Q1, CC_DCOUPL, CHG_TS, BAT_CELL, SPI_MOSI; Phase C unified 10
# signal nets; D1 closed 17 GND links; D2 closed 4 V3_3 links; stitch
# rounds closed BOOST_5V x5, GND x4, USB_DP x1, BAL_A x1, SYS_5V x1).
# The driver tries to close the remainder per the family log's per-net fix
# list (ROUTING_LOG.md §7), in stages, with fail-fast after each stage.
# Family method: blib18b A* (route18b.py + apply18b.py), MF = the variant
# raster. Matrix variants apply one raster to all stages.
#   Stage 1 (Phase H — UNRUN in the family session, lock contention; the
#     command is log §5): A9_PWR, BOOST_5V, GND, V3_3, USB_DN, USB_DP at
#     the log's Phase-H widths (0.25 / 0.25 / 0.3 / 0.3 / 0.25 / 0.25).
#     GND has 5 open entries and V3_3 has 5; both include fenced comps
#     (see EXCLUDED below) that are expected to stay sealed — the router
#     attempts the net, the fence decides the comp.
#   Stage 2 (sealed cluster, one bounded attempt): CC2, ENC_A, ENC_B,
#     ST25_CS, ST25_IRQ, SPI_MISO, IR_TX — all SEALED in the Phase C A*
#     probe at MF 0.02, no fence proof written for any of them. Included
#     because a no-close result fail-fasts and ends the driver honestly;
#     a close on any of them is real progress.
#   Stage 3 (stitch assist): stitch18b.py on the stage-2 DRC — straight /
#     via-hop stitches between DRC pairs, raster-validated. Geometry only
#     (no progress.jsonl lines); skips CHG_PGOOD / ENC_PUSH / ROW3 by rule.
# NOT attempted by this driver:
#   - Any placement change (move C13/C15, L131, C121, C11, R2/J1, the
#     BOOST_5V B track, the SUBGHZ keepout) — all Trevor's call, none
#     attempted. The fenced comps stay open until he rules.
#   - The J-round graze fixes (hand-nudge the four A9_5V In1 vertices,
#     re-route Q1/Q2 at MF 0.05, re-route the D2 V3_3 link off ENC_PUSH,
#     trim the kept GND track at C3.1) — tested fixes failed or untested;
#     the grazes stand in the verdict.
# EXCLUDED by rule, recorded here so the matrix does not decide them:
#   CHG_PGOOD (2), ENC_PUSH (2), ROW3 (1) — frozen bodge nets, close by
#     hand wire, not by copper. Never routed, never stitched.
#   RF_N (1) — FENCED (§6 four-approach proof, tested with both the U9.14
#     jog and RF_P ripped — still sealed). FIX is Trevor's call (move L131
#     0.5 mm north, move C121 0.3 mm east, or rip + re-route SPI_MISO's
#     In2 diagonal).
#   CC_CS (1) — FENCED (§6: only via channel south of U9.7 is 1.20 mm wide
#     vs 1.365 needed for a 0.6 via). FIX is Trevor's call (move the
#     BOOST_5V B track 0.3 mm south, or a 0.4/0.2 prototype-only via).
#   V3_3 comps U9.4 and C15.1 — FENCED (§6: 0 via sites in x39-45,y8-14
#     and x37-44.5,y9-16 respectively). The V3_3 NET is still attempted in
#     stage 1 (its other 3 entries are routable); these two comps are
#     expected to stay sealed. FIX is Trevor's call (move C13/C15).
#   GND pair at (51,1.6) — FENCED by the SUBGHZ_ANTENNA_KEEPOUT (x36-54,
#     y0-3, covers B). Any copper reaching it violates the keepout. The
#     GND NET is still attempted in stage 1 (its other 4 entries are
#     routable); this pair is expected to stay open. FIX is Trevor's call
#     (move the keepout, or accept the open).
# NO DELETIONS except revert: route18b.py and stitch18b.py only add.
# The ONE deletion this driver does is revert_stage(): exact-endpoint
# deletion of a stage's OWN additions only (apply18b.py delete mode,
# per-segment flat pts), and only to undo a stage whose DRC grew clearance
# or shorts vs the previous stage. (Log's stub incident: deleting by bbox /
# endpoint tolerance split a live route. Revert never touches pre-existing
# copper and is DRC-verified — see REVERT below.)
# REVERT (pattern proven by v18d, local verification before landing): the
# stitch tool on a raw board can land closes while adding triple-digit
# clearance (its raster validation is weaker than DRC, same lesson as the
# log's MF-0.02 pour run and the Phase E stitch round: 12 closes for 19
# new grazes + 5 new INA). A* at MF 0.02 can graze the same way. So after
# every stage the driver DRCs, and if clearance or shorts GREW vs the
# previous stage it reverts that stage's additions, re-DRCs, and restores
# the snapshot files (cumulative plan backup + progress.jsonl truncation)
# so the watcher never shows reverted traces. A reverted stage is skipped
# by the gate (counts unchanged) and the next stage runs. If the revert
# itself does not restore the previous counts exactly, the driver stops.
# The revert apply runs with its exit code captured (not through a pipe)
# and retries once on failure: a transient X failure under load made one
# local revert apply silently do nothing (empty output, board unchanged)
# in the v18d landing, which the revert check caught; the retry covers
# that case.
# The board a job finishes with is therefore never worse than baseline
# (31/0/26) on clearance/shorts. Verified locally for v18b before landing
# (see landing report): fail-fast baseline->baseline STOPs, a one-close
# improvement CONTINUES, shorts-grown and clearance-grown STOP, and an
# E2E astar-0.09 run ends no worse than 31/0/26 on clearance/shorts.
# Freerouting variants route whatever is unconnected in the DSN; the DSN
# carries the keepouts, so Freerouting will not cross them.
#
# Snapshot feed: cumulative plan at $SWEEP_PLAN_PATH (default
# $WORKDIR/plan.json), merged per stage (atomic replace); progress.jsonl
# with a start event then one line per finished net (A* stages only —
# stitch contributes geometry, no progress lines); stage DRCs published
# to $WORKDIR/drc.json. Freerouting variants write none of it.
# After each stage: stage DRC + check_failfast.py; on STOP the driver
# stops routing and exits 0 so the harness still records a final DRC.
# (Differs from v18c here: v18c logged STOP and ran the next stage anyway;
# this driver stops, per the driver contract.) Non-zero exit means the
# driver itself broke.
# Stage return codes (astar_stage / stitch_stage): 0 = kept (gate it),
# 2 = reverted (skip gate, counts unchanged, run the next stage),
# 1 = driver error (stop).
set -u
VARIANT=${1:?variant}; BOARD=${2:?board}; WORK=${3:?workdir}
DRIVER_DIR=$(cd "$(dirname "$0")" && pwd)
SWEEP_DIR=$(dirname "$DRIVER_DIR")
REPO_ROOT=$(cd "$DRIVER_DIR/../../.." && pwd)
PLAN_CUM=${SWEEP_PLAN_PATH:-$WORK/plan.json}
PROGRESS=$WORK/progress.jsonl
START_S=$(date +%s)
BUDGET_S=4200   # 70 min: stop starting new stages after this
STAGE_TIMEOUT=1500  # 25 min per route call; route18b.py checkpoints per link

log() { echo "[v18b $VARIANT] $*"; }

# --- helpers ---------------------------------------------------------------
stage_drc() { # <name>: DRC + parse + publish drc.json (live feed)
  local name=$1
  xvfb-run -a python3 "$SWEEP_DIR/drc.py" "$BOARD" "$WORK/drc_$name.txt" >/dev/null 2>&1 || { log "DRC failed at stage $name"; return 1; }
  python3 "$SWEEP_DIR/parse_drc.py" "$WORK/drc_$name.txt" "$WORK/counts_$name.json" >/dev/null
  cp "$WORK/counts_$name.json" "$WORK/drc.json"
  log "stage $name DRC: $(python3 -c "import json; c=json.load(open('$WORK/counts_$name.json')); print(c['unconnected'], 'unc /', c['shorts'], 'shorts /', c['clearance'], 'clearance')")"
}

gate() { # <prev.json> <cur.json>: 0 = CONTINUE, 1 = STOP
  if python3 "$SWEEP_DIR/check_failfast.py" "$1" "$2"; then return 0; else return 1; fi
}

check_gate() { # <counts.json> <stage-name>: CONTINUE updates PREV, STOP exits 0
  if gate "$PREV" "$1"; then PREV="$1"; else log "FAIL-FAST STOP after $2"; exit 0; fi
}

stage_damaged() { # <counts.json>: 0 = clearance/shorts GREW vs PREV, 1 = clean
  # GROWTH vs previous stage, never vs zero: the v18b baseline is 26
  # clearance, so a stage that holds 26 is clean and one that lands 27 is
  # damaged. (check_failfast.py uses the same growth rule.)
  python3 - "$PREV" "$1" <<'PY'
import json, sys
prev = json.load(open(sys.argv[1])); cur = json.load(open(sys.argv[2]))
bad = cur.get("shorts", 0) > prev.get("shorts", 0) or cur.get("clearance", 0) > prev.get("clearance", 0)
print("stage DRC %s/%s/%s vs prev %s/%s/%s -> %s" % (
    cur.get("unconnected"), cur.get("shorts"), cur.get("clearance"),
    prev.get("unconnected"), prev.get("shorts"), prev.get("clearance"),
    "DAMAGED, reverting" if bad else "clean"))
sys.exit(0 if bad else 1)
PY
}

revert_stage() { # <stage-plan> <phase> <prog-lines-before|-1>: 0 = restored, 1 = incomplete
  # Build the delete plan from the stage's OWN additions only. apply18b.py
  # delete mode matches one board track per flat [x1,y1,x2,y2] segment, so
  # polylines are exploded per segment; vias match by net + position.
  local plan=$1 phase=$2 prog_lines=$3
  python3 - "$plan" "$WORK/plan_${phase}_revert.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
d = {"codes": {}, "add_tracks": [], "add_vias": [], "delete": [], "delete_vias": []}
for t in s.get("add_tracks", []):
    pts = t["pts"]
    for i in range(len(pts) - 1):
        d["delete"].append({"net": t["net"], "layer": t["layer"],
                            "pts": [pts[i][0], pts[i][1], pts[i+1][0], pts[i+1][1]]})
for v in s.get("add_vias", []):
    d["delete_vias"].append({"net": v["net"], "at": v["at"]})
json.dump(d, open(sys.argv[2], "w"))
print("revert plan: %d segments + %d vias" % (len(d["delete"]), len(d["delete_vias"])))
PY
  # Apply with output to a log file (not a pipe) so the apply's own exit
  # code survives: a transient X failure must not silently masquerade as
  # an applied plan (the snapshot merge below must only see applied copper).
  xvfb-run -a python3 "$DRIVER_DIR/apply18b.py" "$BOARD" "$WORK/plan_${phase}_revert.json" delete > "$WORK/apply_${phase}_revert.log" 2>&1
  local arc=$?
  grep -v -E 'Warning|GLib|swig' "$WORK/apply_${phase}_revert.log" | tail -2 || true
  if [ "$arc" -ne 0 ] || ! grep -q "^deletions: [1-9]" "$WORK/apply_${phase}_revert.log"; then
    log "revert apply rc=$arc with no deletions logged, retrying once (transient X failure seen 2026-11-26)"
    xvfb-run -a python3 "$DRIVER_DIR/apply18b.py" "$BOARD" "$WORK/plan_${phase}_revert.json" delete > "$WORK/apply_${phase}_revert2.log" 2>&1
    arc=$?
    grep -v -E 'Warning|GLib|swig' "$WORK/apply_${phase}_revert2.log" | tail -2 || true
    log "revert apply retry rc=$arc"
  fi
  stage_drc "${phase}-revert" || return 1
  # Restore the snapshot files to pre-stage state: reverted geometry must
  # never feed the watcher.
  mv "$PLAN_CUM.bak" "$PLAN_CUM"
  if [ "$prog_lines" -ge 0 ]; then head -n "$prog_lines" "$PROGRESS" > "$PROGRESS.tmp" && mv "$PROGRESS.tmp" "$PROGRESS"; fi
  python3 - "$PREV" "$WORK/counts_${phase}-revert.json" <<'PY'
import json, sys
prev = json.load(open(sys.argv[1])); cur = json.load(open(sys.argv[2]))
ok = (prev.get("unconnected") == cur.get("unconnected")
      and prev.get("shorts") == cur.get("shorts")
      and prev.get("clearance") == cur.get("clearance"))
print("revert check: prev %s/%s/%s vs reverted %s/%s/%s -> %s" % (
    prev.get("unconnected"), prev.get("shorts"), prev.get("clearance"),
    cur.get("unconnected"), cur.get("shorts"), cur.get("clearance"),
    "RESTORED" if ok else "INCOMPLETE"))
sys.exit(0 if ok else 1)
PY
}

merge_plan() { # <stage-plan.json>: append into cumulative $PLAN_CUM, atomic
  python3 - "$1" "$PLAN_CUM" <<'PY'
import json, os, sys
stage, cum_path = sys.argv[1], sys.argv[2]
s = json.load(open(stage))
try:
    c = json.load(open(cum_path))
except Exception:
    c = {"codes": {}, "add_tracks": [], "add_vias": [], "delete": [], "delete_vias": []}
c.setdefault("codes", {}).update(s.get("codes", {}))
for k in ("add_tracks", "add_vias", "delete", "delete_vias"):
    c.setdefault(k, []).extend(s.get(k, []))
tmp = cum_path + ".tmp"
with open(tmp, "w") as fh:
    json.dump(c, fh, indent=1)
os.replace(tmp, cum_path)
print("merged %d tracks + %d vias into cumulative plan" % (len(s.get("add_tracks", [])), len(s.get("add_vias", []))))
PY
}

append_progress() { # <route-log>: one progress.jsonl line per finished net
  python3 - "$1" "$PROGRESS" <<'PY'
import json, re, sys
logf, out = sys.argv[1], sys.argv[2]
pat = re.compile(r"^== (\S+): (\d+) links landed, unified=(True|False)")
with open(out, "a") as fh:
    for line in open(logf, errors="replace"):
        m = pat.match(line.strip())
        if m:
            fh.write(json.dumps({"net": m.group(1), "unified": m.group(3) == "True", "links": int(m.group(2))}) + "\n")
PY
}

elapsed() { echo $(( $(date +%s) - START_S )); }

# --- A* stage --------------------------------------------------------------
# astar_stage <phase> <MF> <net:w1,w2>...  returns 0 kept / 2 reverted / 1 error
astar_stage() {
  local phase=$1 mf=$2; shift 2
  [ "$(elapsed)" -gt "$BUDGET_S" ] && { log "time budget spent, skipping stage $phase"; return 1; }
  local plan=$WORK/plan_$phase.json lg=$WORK/log_$phase.txt
  log "stage $phase MF=$mf nets: $*"
  timeout "$STAGE_TIMEOUT" xvfb-run -a python3 "$DRIVER_DIR/route18b.py" "$BOARD" "$plan" "$lg" "$mf" "$@" 2>&1 | grep -v -E 'Warning|GLib|swig' | tail -3
  local trc=${PIPESTATUS[0]}
  log "route $phase rc=$trc (124 = stage timeout, checkpointed plan still applies)"
  if [ -s "$plan" ]; then
    cp "$PLAN_CUM" "$PLAN_CUM.bak"
    local prog_lines=$(wc -l < "$PROGRESS")
    xvfb-run -a python3 "$DRIVER_DIR/apply18b.py" "$BOARD" "$plan" > "$WORK/apply_$phase.log" 2>&1
    local arc=$?
    grep -v -E 'Warning|GLib|swig' "$WORK/apply_$phase.log" | tail -2 || true
    [ "$arc" -ne 0 ] && { log "apply_plan failed rc=$arc, stopping"; return 1; }
    merge_plan "$plan"
    append_progress "$lg"
    stage_drc "$phase" || return 1
    if stage_damaged "$WORK/counts_$phase.json"; then
      log "stage $phase added clearance/shorts, reverting"
      if revert_stage "$plan" "$phase" "$prog_lines"; then
        log "stage $phase reverted, board back to prev counts"
        return 2
      else
        log "REVERT INCOMPLETE after stage $phase, stopping"
        return 1
      fi
    fi
  else
    log "stage $phase produced no plan (all sealed or empty)"
    stage_drc "$phase" || return 1
  fi
  return 0
}

# --- stitch assist stage ---------------------------------------------------
# stitch_stage <prev-drc-txt>: stitch18b.py on a stage DRC report, apply,
# merge, DRC, revert-if-damaged. Returns 0 kept / 2 reverted / 1 error.
stitch_stage() {
  local drc_txt=$1
  [ "$(elapsed)" -gt "$BUDGET_S" ] && { log "time budget spent, skipping stitch assist"; return 1; }
  local plan=$WORK/plan_stitch.json
  log "stitch assist on $drc_txt"
  timeout "$STAGE_TIMEOUT" xvfb-run -a python3 "$DRIVER_DIR/stitch18b.py" "$BOARD" "$drc_txt" "$plan" 2>&1 | grep -v -E 'Warning|GLib|swig' | tail -5
  local trc=${PIPESTATUS[0]}
  log "stitch rc=$trc (124 = timeout, no plan written)"
  if [ -s "$plan" ]; then
    cp "$PLAN_CUM" "$PLAN_CUM.bak"
    xvfb-run -a python3 "$DRIVER_DIR/apply18b.py" "$BOARD" "$plan" > "$WORK/apply_stitch.log" 2>&1
    local arc=$?
    grep -v -E 'Warning|GLib|swig' "$WORK/apply_stitch.log" | tail -2 || true
    [ "$arc" -ne 0 ] && { log "apply_plan failed rc=$arc, stopping"; return 1; }
    merge_plan "$plan"
    stage_drc "stitch" || return 1
    if stage_damaged "$WORK/counts_stitch.json"; then
      log "stitch assist added clearance/shorts, reverting"
      if revert_stage "$plan" "stitch" -1; then
        log "stitch assist reverted, board back to prev counts"
        return 2
      else
        log "REVERT INCOMPLETE after stitch assist, stopping"
        return 1
      fi
    fi
  else
    log "stitch assist produced no plan"
    stage_drc "stitch" || return 1
  fi
  return 0
}

# --- Freerouting stage -----------------------------------------------------
fr_stage() { # <passes>
  local mp=$1
  log "freerouting -mp $mp"
  xvfb-run -a python3 "$SWEEP_DIR/export_dsn.py" "$BOARD" "$WORK/board.dsn" 2>&1 | tail -2
  xvfb-run -a java -jar "$REPO_ROOT/tools/freerouting/freerouting.jar" -de "$WORK/board.dsn" -do "$WORK/board.ses" -mp "$mp" > "$WORK/freerouting.log" 2>&1
  log "freerouting rc=$? (see $WORK/freerouting.log)"
  xvfb-run -a python3 "$SWEEP_DIR/import_ses.py" "$BOARD" "$WORK/board.ses" 2>&1 | tail -3
  stage_drc "fr"
}

# --- phase net lists (family ROUTING_LOG.md §7; widths = family) -----------
# Stage 1: Phase H (log §5 command, unrun). Stage 2: sealed cluster, one
# bounded attempt (family evidence: seals at MF 0.02, no fence proofs).
STAGE_1="A9_PWR:0.25 BOOST_5V:0.25 GND:0.3 V3_3:0.3 USB_DN:0.25 USB_DP:0.25"
STAGE_2="CC2:0.25,0.2 ENC_A:0.25,0.2 ENC_B:0.25,0.2 ST25_CS:0.25,0.2 ST25_IRQ:0.25,0.2 SPI_MISO:0.25,0.2 IR_TX:0.25,0.2"

# run_stage <fn...> : 0 -> gate on <counts>, 2 -> reverted (skip gate), else stop
run_stage() { # <counts-file> <stage-name> <command...>
  local counts=$1 name=$2; shift 2
  "$@"
  local rc=$?
  case $rc in
    0) check_gate "$counts" "$name" ;;
    2) log "$name reverted, continuing" ;;
    *) log "$name driver error, stopping"; exit 0 ;;
  esac
}

case "$VARIANT" in
  astar-0.02|astar-0.05|astar-0.09)
    MF=${VARIANT#astar-}
    export SWEEP_CONGESTION_WEIGHT=${SWEEP_CONGESTION_WEIGHT:-1.5}
    # blib18b needs numpy/PIL/scipy; the workflow installs kicad only, so
    # fetch them here if missing (apt, same lists the workflow updated).
    python3 -c "import numpy, PIL, scipy, wx, pcbnew" 2>/dev/null || {
      log "installing python3-numpy python3-pil python3-scipy for blib18b"
      sudo apt-get install -y -qq python3-numpy python3-pil python3-scipy || { log "DEP FAIL: cannot install blib deps"; exit 1; }
      python3 -c "import numpy, PIL, scipy, wx, pcbnew" 2>/dev/null || { log "DEP FAIL: blib deps still unimportable"; exit 1; }
    }
    NETS_TOTAL=13
    echo "{\"event\": \"start\", \"nets_total\": $NETS_TOTAL}" > "$PROGRESS"
    echo '{"codes": {}, "add_tracks": [], "add_vias": [], "delete": [], "delete_vias": []}' > "$PLAN_CUM"
    PREV="$WORK/baseline_counts.json"
    # The harness writes $WORK/baseline_counts.json before calling the
    # driver. If it is missing, fall back to the FROZEN v18b baseline
    # 31/0/26 — never 0 clearance, or the first gate STOPs on the
    # baseline's own 26.
    [ -f "$PREV" ] || { log "no baseline counts at $PREV; using frozen v18b baseline 31/0/26"; echo '{"unconnected": 31, "shorts": 0, "clearance": 26}' > "$PREV"; }
    run_stage "$WORK/counts_1.json" "stage 1" astar_stage "1" "$MF" $STAGE_1
    run_stage "$WORK/counts_2.json" "stage 2" astar_stage "2" "$MF" $STAGE_2
    run_stage "$WORK/counts_stitch.json" "stitch assist" stitch_stage "$WORK/drc_2.txt"
    log "astar $VARIANT done"
    ;;
  freerouting-mp1|freerouting-mp3|freerouting-mp10)
    MP=${VARIANT#freerouting-mp}
    [ -f "$REPO_ROOT/tools/freerouting/freerouting.jar" ] || { log "DEP FAIL: freerouting jar missing at tools/freerouting/freerouting.jar"; exit 1; }
    fr_stage "$MP"
    log "freerouting $VARIANT done"
    ;;
  *)
    log "unknown variant: $VARIANT"
    exit 1
    ;;
esac
log "driver done, elapsed $(elapsed)s"
exit 0
