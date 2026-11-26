#!/bin/bash
# v18c-r3 family driver — IMP-1 Main placement sweep (GitHub Actions).
# Contract (tools/sweep/README.md, "Family driver interface" + "A* driver
# snapshot contract (live feed)"):
#   run.sh <variant> <board.kicad_pcb> <workdir>
# Variants: astar-0.02, astar-0.05, astar-0.09,
#           freerouting-mp1, freerouting-mp3, freerouting-mp10.
#
# v18c-r3 is NOT a ripped input — it is a routed board at 11/0/0, the NEW
# BEST banked board: it BEATS v18c's 13/0/0 and v15's 15/0/0 (Gate 3 still
# NOT PASS). Banked 2026-11-25 22:50 CST from a pristine v18c copy by the
# apply script in files/pcb/v5/v18c-r3/v18c-r3.md section 3 (log + probes
# in that dir; board md5 7ab878d6fec875e5f708d1ddfff678e1).
# What r3 closed vs v18c (13 -> 11): ENC_PUSH x2, A9_5V, ROW3. What it
# opened: 2 NEW V3_3 pairs inside the 11 — pour splits from the new In2
# copper (J14.1 (58.56,57.8), J3.1 (37.0,3.2); log section 6, unmapped
# necks, UNPROBED fixes).
# The driver tries to close the remaining 11 per the r3 log's per-net state
# (v18c-r3.md section 6), in stages, with fail-fast after each stage.
# Family method: blib18cr3 A* (route18cr3.py + apply18cr3.py), MF = the
# variant raster. Matrix variants apply one raster to all stages.
#   Stage 1 (cage): CC_CS, CC_DCOUPL, CC_XOSC_Q1
#   Stage 2 (pours): V3_3 only (A9_5V is CLOSED in r3 — do not re-add it;
#     the v18c stage-2 list "V3_3, A9_5V" does NOT transfer).
#   Handvia pour assist after stage 2: targeted handvia on GND/V3_3 pads
#     still unconnected per the stage-2 DRC (v18c Phase 4 method).
# All three CC nets are UNVERIFIED / sealed per the r3 log, routed as-is:
#   CC_CS — no path at 0.20 with the via fixed at (43.47,11.65); partial
#     route clean to (29.00,20.75), 17.09 mm short; A* exhaustive fail.
#   CC_DCOUPL — no via site clears 0.20 on all 4 layers (best midpoint
#     via fails In2 -0.385 vs the ST25_CS horizontal at y10.3).
#   CC_XOSC_Q1 — no one-via site clears 0.20 (best complete attempt min
#     0.071). V3_3 U9.18 is BLOCKED (copper surgery first, log section 2.12
#     U9.18); the J3.1/J14.1 pour splits are unmapped. A no-close result
#     fail-fasts and ends the driver honestly; a close is real progress.
# NOTE on the CC unlock (family fix list): the listed via move
# (43.47,11.65) -> (42.9,11.65) was TESTED and REJECTED — on the final
# board the destination grazes 7 neighbours and the stub deletion splits
# CC_CS (+1 unconnected). The r3 log re-confirms it dead for CC_XOSC_Q1
# (F -0.103, In1 -0.137). The driver does NOT move the via.
# TRACE-PLAN CORRECTION (r3 log, carried here so the matrix does not act
# on it): the plan's "Ready now (4 of 13)" — CC_DCOUPL, CC_XOSC_Q1,
# CHG_TS, USB_DP — is 0 of 4 after re-measurement. All four are
# UNVERIFIED. None of the four is treated as closable in this driver.
# BLIND-VIA NOTE (r3 log section 6): pcbnew SetLayerPair alone is silently
# ignored in KiCad 7.0.11 — script-built vias come out THRU unless
# SetViaType(1) is called FIRST. The sweep A* applier (apply18cr3.py)
# adds THRU vias only (SetViaType(3)); the r3 board's ENC_PUSH blind vias
# are already-landed copper and are never touched by this driver. Do not
# "fix" the applier to blind vias without carrying the SetViaType-first
# ordering — the first r3 test-apply proved the failure mode (37 clearance
# + 34 hole + 2 mask violations from thru-built "blind" vias).
# EXCLUDED by rule, recorded here so the matrix does not decide them:
#   CHG_TS, USB_DP — corridors run through the SUBGHZ keepout band
#     (y0-3). The 2026-11-25 ruling allows inner-layer crossings, but the
#     r3 log proves both UNROUTABLE on this board anyway: CHG_TS's In1
#     link (54.55,5.9) -> (55.95,3.35) has NO path (analytic seal — the
#     SPI_MOSI/SPI_MISO via clearance circles overlap); USB_DP's tail
#     from via (38.85,4.6) to the west comp is A*-proven unroutable in
#     three bboxes, and its F y1.95 leg crosses the band on F.Cu, which
#     the ruling as recorded covers only for inner layers.
#   CHG_PGOOD (2) — frozen bodge net, close by hand wire, not by copper.
#     Leg 1: U3.7 is F-trapped (best via 0.069). Leg 2: the straight
#     diagonal was WITHDRAWN — it crosses the board cutout (x36-103,
#     y18-56); a re-route around the cutout is UNPROBED.
#   SPI_MISO — frozen bodge net. Dogleg clean (min 0.211) but no via spot
#     meets 0.20 on all layers (best 0.100 / 0.150); onward exits all fail.
#   ENC_PUSH (2), A9_5V, ROW3 — CLOSED in r3; not in the open set at all.
# NO DELETIONS except revert: route18cr3.py and handvia18cr3.py only add.
# The ONE deletion this driver does is revert_stage(): exact-endpoint
# deletion of a stage's OWN additions only (apply18cr3.py delete mode,
# per-segment flat pts), and only to undo a stage whose DRC grew clearance
# or shorts. (Log's stub incident: deleting by bbox / endpoint tolerance
# split a live route. Revert never touches pre-existing copper and is
# DRC-verified — see REVERT below.)
# REVERT (pattern proven by v18d/v18b, local verification before landing):
# the stitch tool on a raw board can land closes while adding triple-digit
# clearance (its raster validation is weaker than DRC, same lesson as the
# log's MF-0.02 pour run). A* at MF 0.02 can graze the same way. So after
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
# (11/0/0) on clearance/shorts. Verified locally for v18c-r3 before
# landing (see landing report): fail-fast shorts-grown and clearance-grown
# STOP, and an E2E astar-0.09 run ends no worse than 11/0/0 on
# clearance/shorts.
# FAIL-FAST TARGET NOTE (harness-owner decision, not this driver): the
# repo's check_failfast.py still fail-fasts against v15's 15 ("no
# improvement while >= 15"). At 11/0/0 a no-close stage CONTINUES under
# that gate (11 < 15) — the gate only still bites on shorts appearing or
# clearance growing. The new best is 11, so the target the gate should
# carry is 11: STOP when a stage closes nothing while still at >= 11.
# This driver calls the repo gate as written (driver contract) and does
# not reimplement it; until the harness owner retargets it, a sealed
# stage 1 continues to stage 2 rather than stopping the driver there.
# .PRO NOTE: the staged imp1_main_v18c_r3.kicad_pro is v18c-r3's OWN
# project file (md5 cca4661e24eb2c34ab2b69e70d9c08fb) — the same bytes as
# the v15/v18a/v18c/v18d pattern .pro, and it reproduces the frozen DRC
# exactly (11/0/0, total 430). The v18b lesson (family-pattern .pro does
# NOT reproduce a frozen DRC) does not bite here because own == pattern
# for this board; the frozen verdict is the authority either way.
# Freerouting variants route whatever is unconnected in the DSN; the DSN
# carries the keepouts, so Freerouting will not cross them.
#
# Snapshot feed: cumulative plan at $SWEEP_PLAN_PATH (default
# $WORKDIR/plan.json), merged per stage (atomic replace); progress.jsonl
# with a start event then one line per finished net (A* stages only —
# handvia contributes geometry, no progress lines); stage DRCs published
# to $WORKDIR/drc.json. Freerouting variants write none of it.
# After each stage: stage DRC + check_failfast.py; on STOP the driver
# stops routing and exits 0 so the harness still records a final DRC.
# (Differs from v18c here: v18c logged STOP and ran the next stage anyway;
# this driver stops, per the driver contract.) Non-zero exit means the
# driver itself broke.
# Stage return codes (astar_stage / handvia_stage): 0 = kept (gate it),
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
BUDGET_S=${SWEEP_BUDGET_S:-4200}   # 70 min: stop starting new stages after this
STAGE_TIMEOUT=${SWEEP_STAGE_TIMEOUT:-1500}  # 25 min per route call; route18cr3.py checkpoints per link

log() { echo "[v18c-r3 $VARIANT] $*"; }

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
  # GROWTH vs previous stage, never vs zero (the v18b lesson — a baseline
  # with pre-existing clearance must not read as damage; r3's baseline is
  # 0 clearance, so the rule coincides with vs-zero here, but the
  # growth-vs-prev form is the one that stays correct if that changes).
  # (check_failfast.py uses the same growth rule.)
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
  # Build the delete plan from the stage's OWN additions only. apply18cr3.py
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
  xvfb-run -a python3 "$DRIVER_DIR/apply18cr3.py" "$BOARD" "$WORK/plan_${phase}_revert.json" delete > "$WORK/apply_${phase}_revert.log" 2>&1
  local arc=$?
  grep -v -E 'Warning|GLib|swig' "$WORK/apply_${phase}_revert.log" | tail -2 || true
  if [ "$arc" -ne 0 ] || ! grep -q "^deletions: [1-9]" "$WORK/apply_${phase}_revert.log"; then
    log "revert apply rc=$arc with no deletions logged, retrying once (transient X failure seen 2026-11-26)"
    xvfb-run -a python3 "$DRIVER_DIR/apply18cr3.py" "$BOARD" "$WORK/plan_${phase}_revert.json" delete > "$WORK/apply_${phase}_revert2.log" 2>&1
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
  timeout "$STAGE_TIMEOUT" xvfb-run -a python3 "$DRIVER_DIR/route18cr3.py" "$BOARD" "$plan" "$lg" "$mf" "$@" 2>&1 | grep -v -E 'Warning|GLib|swig' | tail -3
  local trc=${PIPESTATUS[0]}
  log "route $phase rc=$trc (124 = stage timeout, checkpointed plan still applies)"
  if [ -s "$plan" ]; then
    cp "$PLAN_CUM" "$PLAN_CUM.bak"
    local prog_lines=$(wc -l < "$PROGRESS")
    xvfb-run -a python3 "$DRIVER_DIR/apply18cr3.py" "$BOARD" "$plan" > "$WORK/apply_$phase.log" 2>&1
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

# --- handvia pour assist stage ---------------------------------------------
# handvia_stage <prev-drc-txt>: handvia18cr3.py on a stage DRC report —
# targeted vias on GND/V3_3 pads still unconnected (v18c Phase 4 method).
# Apply, merge, DRC, revert-if-damaged. Returns 0 kept / 2 reverted / 1 error.
handvia_stage() {
  local drc_txt=$1
  [ "$(elapsed)" -gt "$BUDGET_S" ] && { log "time budget spent, skipping handvia assist"; return 1; }
  local plan=$WORK/plan_handvia.json
  log "handvia pour assist on $drc_txt"
  # Targets: ONLY pads named inside [unconnected_items] sections, on the
  # pour nets. The v18c extraction scanned the whole DRC, which sweeps in
  # pads that merely sit in a keepout (J1 GND, J11, D1.1 — all from
  # [items_not_allowed], none unconnected) and misses the pads that ARE
  # open: J3.1 is reported as "PTH pad" (lowercase) and J14.1 is reported
  # as a Track stub in some runs (same island, opposite-end reporting —
  # see the landing report), so neither matches a bare "Pad" scan.
  python3 - "$drc_txt" "$WORK/handvia_targets.txt" <<'PY'
import re, sys
pat = re.compile(r"(?:PTH )?pad (\S+) \[([^\]]+)\] of (\S+)", re.IGNORECASE)
sec = re.compile(r"^\[([a-z_0-9]+)\]")
out = set()
in_unc = False
for line in open(sys.argv[1], errors="replace"):
    m = sec.match(line)
    if m:
        in_unc = (m.group(1) == "unconnected_items")
        continue
    if not in_unc:
        continue
    m = pat.search(line)
    if m and m.group(2) in ("GND", "V3_3"):
        out.add("%s:%s.%s" % (m.group(2), m.group(3), m.group(1)))
open(sys.argv[2], "w").write("\n".join(sorted(out)))
PY
  if [ ! -s "$WORK/handvia_targets.txt" ]; then
    log "handvia assist: no GND/V3_3 pads open, skipping"
    stage_drc "handvia" || return 1
    return 0
  fi
  log "handvia pour assist on $(wc -l < "$WORK/handvia_targets.txt") pads"
  timeout "$STAGE_TIMEOUT" xvfb-run -a python3 "$DRIVER_DIR/handvia18cr3.py" "$BOARD" "$plan" $(cat "$WORK/handvia_targets.txt") 2>&1 | grep -v -E 'Warning|GLib|swig' | tail -2
  local trc=${PIPESTATUS[0]}
  log "handvia rc=$trc (124 = timeout, no plan written)"
  if [ -s "$plan" ]; then
    cp "$PLAN_CUM" "$PLAN_CUM.bak"
    xvfb-run -a python3 "$DRIVER_DIR/apply18cr3.py" "$BOARD" "$plan" > "$WORK/apply_handvia.log" 2>&1
    local arc=$?
    grep -v -E 'Warning|GLib|swig' "$WORK/apply_handvia.log" | tail -2 || true
    [ "$arc" -ne 0 ] && { log "apply_plan failed rc=$arc, stopping"; return 1; }
    merge_plan "$plan"
    stage_drc "handvia" || return 1
    if stage_damaged "$WORK/counts_handvia.json"; then
      log "handvia assist added clearance/shorts, reverting"
      if revert_stage "$plan" "handvia" -1; then
        log "handvia assist reverted, board back to prev counts"
        return 2
      else
        log "REVERT INCOMPLETE after handvia assist, stopping"
        return 1
      fi
    fi
  else
    log "handvia assist produced no plan"
    stage_drc "handvia" || return 1
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

# --- phase net lists (r3 log v18c-r3.md section 6; widths = v18c family) ---
# Stage 1: the CC cage (all UNVERIFIED/sealed per the log — attempted
# as-is, the fence decides). Stage 2: V3_3 only — A9_5V is CLOSED in r3.
STAGE_1="CC_CS:0.25,0.2 CC_DCOUPL:0.25,0.2 CC_XOSC_Q1:0.25,0.2"
STAGE_2="V3_3:0.5,0.4"

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
    # blib18cr3 needs numpy/PIL/scipy; the workflow installs kicad only, so
    # fetch them here if missing (apt, same lists the workflow updated).
    python3 -c "import numpy, PIL, scipy, wx, pcbnew" 2>/dev/null || {
      log "installing python3-numpy python3-pil python3-scipy for blib18cr3"
      sudo apt-get install -y -qq python3-numpy python3-pil python3-scipy || { log "DEP FAIL: cannot install blib deps"; exit 1; }
      python3 -c "import numpy, PIL, scipy, wx, pcbnew" 2>/dev/null || { log "DEP FAIL: blib deps still unimportable"; exit 1; }
    }
    NETS_TOTAL=4
    echo "{\"event\": \"start\", \"nets_total\": $NETS_TOTAL}" > "$PROGRESS"
    echo '{"codes": {}, "add_tracks": [], "add_vias": [], "delete": [], "delete_vias": []}' > "$PLAN_CUM"
    PREV="$WORK/baseline_counts.json"
    # The harness writes $WORK/baseline_counts.json before calling the
    # driver. If it is missing, fall back to the FROZEN v18c-r3 baseline
    # 11/0/0 — never 999/0/0 with a stale threshold, or the first gate
    # misreads the board it is standing on.
    [ -f "$PREV" ] || { log "no baseline counts at $PREV; using frozen v18c-r3 baseline 11/0/0"; echo '{"unconnected": 11, "shorts": 0, "clearance": 0}' > "$PREV"; }
    run_stage "$WORK/counts_1.json" "stage 1" astar_stage "1" "$MF" $STAGE_1
    run_stage "$WORK/counts_2.json" "stage 2" astar_stage "2" "$MF" $STAGE_2
    run_stage "$WORK/counts_handvia.json" "handvia assist" handvia_stage "$WORK/drc_2.txt"
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
