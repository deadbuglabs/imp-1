#!/bin/bash
# v18d family driver — IMP-1 Main placement sweep (GitHub Actions).
# Contract (tools/sweep/README.md, "Family driver interface" + "A* driver
# snapshot contract (live feed)"):
#   run.sh <variant> <board.kicad_pcb> <workdir>
# Variants: astar-0.02, astar-0.05, astar-0.09,
#           freerouting-mp1, freerouting-mp3, freerouting-mp10.
#
# v18d is NOT a ripped input — it is a routed board at 47/0/0 (WORSE than
# v15's 15/0/0 on the headline; Gate 3 NOT PASS, NOT fab-ready). The outline
# moves OPEN the corridors the v15 fence items needed (BOOST_5V, A9_5V via
# the new east channel, CC_DCOUPL, CC_XOSC_Q2, SPI_SCLK/MOSI, ST25_IRQ,
# DISP_DC all closed in the family run); the bulk re-route is incomplete —
# 47 open, dominated by pour-net pads (GND 12 + V3_3 5) and sealed nets.
# The driver tries to close the remainder per the family log's per-net fix
# list (ROUTING_LOG.md, REMAINDER), in stages, with fail-fast after each
# stage. Family method: blib18d A* (route18d.py + apply18d.py), MF = the
# variant raster. Matrix variants apply one raster to all stages.
#   Stage 1 (signal FIXes): A9_PWR (re-lay the (32.3,27.85)->U1.12 link the
#     stub incident deleted; B3 geometry already re-applied), CC_XOSC_Q1
#     (U9.8->C11.1/X1.1; probe-passed at 0.02 on the empty base), CC_CS
#     (B-layer south-leg candidate, unverified), IR_RX_OUT (re-lay at 0.09
#     via the south band, NOT the grazed north-strip 0.02 thread).
#   Stage 2 (pours): GND, V3_3 at the family's Phase-D widths (0.25, 0.2
#     fallback). U9.4/9/18 are QFN-cage FENCEs; C18.1 + U9.14/15 have
#     hand-via candidates ((37.2,8.8), (45.8,8.6)) the A* may find.
#   Stage 3 (stitch assist): stitch18d.py on the stage-2 DRC — straight /
#     via-hop stitches between DRC pairs, raster-validated. Geometry only
#     (no progress.jsonl lines); skips CHG_PGOOD / ENC_PUSH / ROW3 by rule.
#   Stage 4 (sealed cluster, one bounded attempt): BAL_B, BAL_BDC, BAL_E,
#     RF_ANT, RF_P, RF_N, CC1, CHG_TMR, USB_DN, USB_DP, VBUS_5V, SPI_MISO.
#     Family evidence says these seal (BAL needs the surgery below; CC1 /
#     CHG_TMR / USB_DN/DP / VBUS_5V are structural fences; SPI_MISO is the
#     H1-keepout fence). Included because the board changed since the seals
#     were measured (D-phase pours landed); a no-close result fail-fasts
#     and ends the driver.
# NOT attempted by this driver (planned but NOT RUN by the family worker):
#   - MISO corridor attempt (log's documented path: F west from U9.2
#     (42.06,8.50) to x40.0, south to y13.5, west at y16.5 to x28.0, via
#     (28.0,15.8), In2 south to y27.91, via (28.0,27.91), F east into
#     U1.19) — left to r1/r2 and the Freerouting variants.
#   - BAL surgery (rip BOOST_5V's D2.1 diagonal (61.45,7.05)->(64.15,5.65)
#     + stubs, re-lay D2.1 south of the ladder, route BAL_B at w0.20 west
#     at y6.5, then BAL_BDC/E + RF_ANT) — touches a closed net; needs a
#     deletion + re-lay the driver does not do (see below).
# EXCLUDED by rule, recorded here so the matrix does not decide them:
#   CHG_PGOOD (2), ENC_PUSH (3) — accepted bodges, whitelisted from the
#     rip. ROW3 (1) — frozen v15 item, never routed.
# NO DELETIONS except revert: route18d.py and stitch18d.py only add.
# The ONE deletion this driver does is revert_stage(): exact-endpoint
# deletion of a stage's OWN additions only (apply18d.py delete mode,
# per-segment flat pts), and only to undo a stage whose DRC grew clearance
# or shorts. (Log's stub incident: deleting by bbox / endpoint tolerance
# split a live route, 47->51. Revert never touches pre-existing copper and
# is DRC-verified — see REVERT below.)
# REVERT (measured 2026-11-26, local verification before landing): the
# stitch tool on the raw 47/0/0 board emits 28 tracks + 16 vias and lands
# 47->34 unconnected but adds 123 clearance (its raster validation is
# weaker than DRC, same lesson as the log's MF 0.02 pour run: 9 closes
# for 11 grazes). A* at MF 0.02 can graze the same way. So after every
# stage the driver DRCs, and if clearance or shorts GREW vs the previous
# stage it reverts that stage's additions, re-DRCs, and restores the
# snapshot files (cumulative plan backup + progress.jsonl truncation) so
# the watcher never shows reverted traces. A reverted stage is skipped
# by the gate (counts unchanged) and the next stage runs. If the revert
# itself does not restore the previous counts exactly, the driver stops.
# The revert apply runs with its exit code captured (not through a pipe)
# and retries once on failure: a transient X failure under load made one
# local revert apply silently do nothing (empty output, board unchanged),
# which the revert check caught; the retry covers that case.
# The board a job finishes with is therefore never worse than baseline
# on clearance/shorts. Verified locally: stitch 47/0/0 -> 34/0/123 ->
# revert -> 47/0/0 exactly.
# OUTLINE DEVIATIONS (need Trevor's ruling + case v3.7 rework; NOT
# re-litigated here — the matrix runs on the board as delivered):
#   bay +1.43 S, board east +3.0, north +1.0, south +0.8; J12 +0.95 S,
#   south band +1.0, J2 +1.43 S, balun ladder +0.5..+1.5 E, boost +1.0..+1.5 E.
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
STAGE_TIMEOUT=1500  # 25 min per route call; route18d.py checkpoints per link

log() { echo "[v18d $VARIANT] $*"; }

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
  # Build the delete plan from the stage's OWN additions only. apply18d.py
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
  xvfb-run -a python3 "$DRIVER_DIR/apply18d.py" "$BOARD" "$WORK/plan_${phase}_revert.json" delete > "$WORK/apply_${phase}_revert.log" 2>&1
  local arc=$?
  grep -v -E 'Warning|GLib|swig' "$WORK/apply_${phase}_revert.log" | tail -2 || true
  if [ "$arc" -ne 0 ] || ! grep -q "^deletions: [1-9]" "$WORK/apply_${phase}_revert.log"; then
    log "revert apply rc=$arc with no deletions logged, retrying once (transient X failure seen 2026-11-26)"
    xvfb-run -a python3 "$DRIVER_DIR/apply18d.py" "$BOARD" "$WORK/plan_${phase}_revert.json" delete > "$WORK/apply_${phase}_revert2.log" 2>&1
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
  timeout "$STAGE_TIMEOUT" xvfb-run -a python3 "$DRIVER_DIR/route18d.py" "$BOARD" "$plan" "$lg" "$mf" "$@" 2>&1 | grep -v -E 'Warning|GLib|swig' | tail -3
  local trc=${PIPESTATUS[0]}
  log "route $phase rc=$trc (124 = stage timeout, checkpointed plan still applies)"
  if [ -s "$plan" ]; then
    cp "$PLAN_CUM" "$PLAN_CUM.bak"
    local prog_lines=$(wc -l < "$PROGRESS")
    xvfb-run -a python3 "$DRIVER_DIR/apply18d.py" "$BOARD" "$plan" > "$WORK/apply_$phase.log" 2>&1
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
# stitch_stage <prev-drc-txt>: stitch18d.py on a stage DRC report, apply,
# merge, DRC, revert-if-damaged. Returns 0 kept / 2 reverted / 1 error.
stitch_stage() {
  local drc_txt=$1
  [ "$(elapsed)" -gt "$BUDGET_S" ] && { log "time budget spent, skipping stitch assist"; return 1; }
  local plan=$WORK/plan_stitch.json
  log "stitch assist on $drc_txt"
  timeout "$STAGE_TIMEOUT" xvfb-run -a python3 "$DRIVER_DIR/stitch18d.py" "$BOARD" "$drc_txt" "$plan" 2>&1 | grep -v -E 'Warning|GLib|swig' | tail -5
  local trc=${PIPESTATUS[0]}
  log "stitch rc=$trc (124 = timeout, no plan written)"
  if [ -s "$plan" ]; then
    cp "$PLAN_CUM" "$PLAN_CUM.bak"
    xvfb-run -a python3 "$DRIVER_DIR/apply18d.py" "$BOARD" "$plan" > "$WORK/apply_stitch.log" 2>&1
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

# --- phase net lists (family ROUTING_LOG.md REMAINDER; widths = family) -----
# Stage 1: signal FIXes. Stage 2: pours. Stage 4: sealed cluster, one
# bounded attempt (family evidence: seals; board changed since, so try once).
STAGE_1="A9_PWR:0.25 CC_XOSC_Q1:0.2 CC_CS:0.25 IR_RX_OUT:0.25"
STAGE_2="GND:0.25,0.2 V3_3:0.25,0.2"
STAGE_4="BAL_B:0.25,0.2 BAL_BDC:0.25,0.2 BAL_E:0.25,0.2 RF_ANT:0.25,0.2 RF_P:0.25,0.2 RF_N:0.25,0.2 CC1:0.25,0.2 CHG_TMR:0.25,0.2 USB_DN:0.25,0.2 USB_DP:0.25,0.2 VBUS_5V:0.25,0.2 SPI_MISO:0.25,0.2"

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
    # blib18d needs numpy/PIL/scipy; the workflow installs kicad only, so
    # fetch them here if missing (apt, same lists the workflow updated).
    python3 -c "import numpy, PIL, scipy, wx, pcbnew" 2>/dev/null || {
      log "installing python3-numpy python3-pil python3-scipy for blib18d"
      sudo apt-get install -y -qq python3-numpy python3-pil python3-scipy || { log "DEP FAIL: cannot install blib deps"; exit 1; }
      python3 -c "import numpy, PIL, scipy, wx, pcbnew" 2>/dev/null || { log "DEP FAIL: blib deps still unimportable"; exit 1; }
    }
    NETS_TOTAL=18
    echo "{\"event\": \"start\", \"nets_total\": $NETS_TOTAL}" > "$PROGRESS"
    echo '{"codes": {}, "add_tracks": [], "add_vias": [], "delete": [], "delete_vias": []}' > "$PLAN_CUM"
    PREV="$WORK/baseline_counts.json"
    [ -f "$PREV" ] || { log "no baseline counts at $PREV; using empty prev"; echo '{"unconnected": 999, "shorts": 0, "clearance": 0}' > "$PREV"; }
    run_stage "$WORK/counts_1.json" "stage 1" astar_stage "1" "$MF" $STAGE_1
    run_stage "$WORK/counts_2.json" "stage 2" astar_stage "2" "$MF" $STAGE_2
    run_stage "$WORK/counts_stitch.json" "stitch assist" stitch_stage "$WORK/drc_2.txt"
    run_stage "$WORK/counts_4.json" "stage 4" astar_stage "4" "$MF" $STAGE_4
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
