#!/bin/bash
# v18c family driver — IMP-1 Main placement sweep (GitHub Actions).
# Contract (tools/sweep/README.md, "Family driver interface" + "A* driver
# snapshot contract (live feed)"):
#   run.sh <variant> <board.kicad_pcb> <workdir>
# Variants: astar-0.02, astar-0.05, astar-0.09,
#           freerouting-mp1, freerouting-mp3, freerouting-mp10.
#
# v18c is NOT a ripped input — it is a routed board at 13/0/0 (BEATS v15's
# 15/0/0; Gate 3 still NOT PASS). The driver tries to close the last 13.
# Family method: blib A* (route17.py + apply_plan.py), MF 0.02 for QFN-exit
# stubs, MF 0.09 bulk. Matrix variants apply one raster to all stages.
#
# NOTE on the CC unlock (family fix list): the listed via move
# (43.47,11.65) -> (42.9,11.65) was TESTED and REJECTED — on the final
# board the destination grazes 7 neighbours (CC_DCOUPL F tracks at 0.0mm,
# SPI_MISO In1 tracks at 0.0mm, U9 pad 6, GND B tracks) and the stub
# deletion splits CC_CS (+1 unconnected: 13/0/0 -> 14/0/7). The destination
# needs re-derivation with probes; the driver does NOT move the via.
# The CC trio is routed as-is and will likely seal (family evidence).
#   Stage 1 (cage): CC_CS, CC_DCOUPL, CC_XOSC_Q1
#   Stage 2 (pours + power): V3_3, A9_5V
# EXCLUDED by rule, recorded here so the matrix does not decide them:
#   CHG_TS, USB_DP — corridors run through the SUBGHZ keepout band (y0-3);
#     need Trevor's keepout ruling (family log REMAINDER).
#   CHG_PGOOD, ENC_PUSH, SPI_MISO — v15 bodges, untouched by rule.
#   ROW3 — frozen v15 item.
# Freerouting variants route whatever is unconnected in the DSN; the DSN
# carries the keepout, so Freerouting will not cross the SUBGHZ band.
#
# Snapshot feed: cumulative plan at $SWEEP_PLAN_PATH (default
# $WORKDIR/plan.json), merged per stage (atomic replace); progress.jsonl
# with a start event then one line per finished net; stage DRCs published
# to $WORKDIR/drc.json. Freerouting variants write none of it.
# After each stage: stage DRC + check_failfast.py; on STOP, stop routing
# and exit 0 so the harness still records a final DRC. Non-zero exit means
# the driver itself broke.
set -u
VARIANT=${1:?variant}; BOARD=${2:?board}; WORK=${3:?workdir}
DRIVER_DIR=$(cd "$(dirname "$0")" && pwd)
SWEEP_DIR=$(dirname "$DRIVER_DIR")
REPO_ROOT=$(cd "$DRIVER_DIR/../../.." && pwd)
PLAN_CUM=${SWEEP_PLAN_PATH:-$WORK/plan.json}
PROGRESS=$WORK/progress.jsonl
START_S=$(date +%s)
BUDGET_S=4200   # 70 min: stop starting new stages after this
STAGE_TIMEOUT=1500  # 25 min per route call; route17.py checkpoints per link

log() { echo "[v18c $VARIANT] $*"; }

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
# astar_stage <phase> <MF> <net:w1,w2>...
astar_stage() {
  local phase=$1 mf=$2; shift 2
  [ "$(elapsed)" -gt "$BUDGET_S" ] && { log "time budget spent, skipping stage $phase"; return 1; }
  local plan=$WORK/plan_$phase.json lg=$WORK/log_$phase.txt
  log "stage $phase MF=$mf nets: $*"
  timeout "$STAGE_TIMEOUT" xvfb-run -a python3 "$DRIVER_DIR/route17.py" "$BOARD" "$plan" "$lg" "$mf" "$@" 2>&1 | grep -v -E 'Warning|GLib|swig' | tail -3
  log "route $phase rc=$? (124 = stage timeout, checkpointed plan still applies)"
  if [ -s "$plan" ]; then
    xvfb-run -a python3 "$DRIVER_DIR/apply_plan.py" "$BOARD" "$plan" 2>&1 | grep -v -E 'Warning|GLib|swig' | tail -2
    merge_plan "$plan"
    append_progress "$lg"
  else
    log "stage $phase produced no plan (all sealed or empty)"
  fi
  stage_drc "$phase" || return 1
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

# --- phase net lists --------------------------------------------------------
STAGE_1="CC_CS:0.25,0.2 CC_DCOUPL:0.25,0.2 CC_XOSC_Q1:0.25,0.2"
STAGE_2="V3_3:0.5,0.4 A9_5V:0.5,0.4"

case "$VARIANT" in
  astar-0.02|astar-0.05|astar-0.09)
    MF=${VARIANT#astar-}
    export SWEEP_CONGESTION_WEIGHT=${SWEEP_CONGESTION_WEIGHT:-1.5}
    python3 -c "import numpy, PIL, scipy, wx, pcbnew" 2>/dev/null || {
      log "installing python3-numpy python3-pil python3-scipy for blib"
      sudo apt-get install -y -qq python3-numpy python3-pil python3-scipy || { log "DEP FAIL: cannot install blib deps"; exit 1; }
      python3 -c "import numpy, PIL, scipy, wx, pcbnew" 2>/dev/null || { log "DEP FAIL: blib deps still unimportable"; exit 1; }
    }
    NETS_TOTAL=5
    echo "{\"event\": \"start\", \"nets_total\": $NETS_TOTAL}" > "$PROGRESS"
    echo '{"codes": {}, "add_tracks": [], "add_vias": [], "delete": [], "delete_vias": []}' > "$PLAN_CUM"
    PREV="$WORK/baseline_counts.json"
    [ -f "$PREV" ] || { log "no baseline counts at $PREV; using empty prev"; echo '{"unconnected": 999, "shorts": 0, "clearance": 0}' > "$PREV"; }
    astar_stage "1" "$MF" $STAGE_1 || { log "stage 1 driver error, stopping"; }
    if [ -f "$WORK/counts_1.json" ]; then
      if gate "$PREV" "$WORK/counts_1.json"; then PREV="$WORK/counts_1.json"; else log "FAIL-FAST STOP after stage 1"; fi
    fi
    astar_stage "2" "$MF" $STAGE_2 || { log "stage 2 driver error, stopping"; }
    if [ -f "$WORK/counts_2.json" ]; then
      if gate "$PREV" "$WORK/counts_2.json"; then PREV="$WORK/counts_2.json"; else log "FAIL-FAST STOP after stage 2"; fi
    fi
    # Pour assist: targeted handvia on GND/V3_3 pads still unconnected per
    # the last stage DRC (family Phase 4 method).
    if [ "$(elapsed)" -lt "$BUDGET_S" ] && [ -f "$WORK/drc_2.txt" ]; then
      python3 - "$WORK/drc_2.txt" "$WORK/handvia_targets.txt" <<'PY'
import re, sys
pat = re.compile(r"Pad (\S+) \[([^\]]+)\] of (\S+)")
out = set()
for line in open(sys.argv[1], errors="replace"):
    m = pat.search(line)
    if m and m.group(2) in ("GND", "V3_3"):
        out.add("%s:%s.%s" % (m.group(2), m.group(3), m.group(1)))
open(sys.argv[2], "w").write("\n".join(sorted(out)))
PY
      if [ -s "$WORK/handvia_targets.txt" ]; then
        log "handvia pour assist on $(wc -l < "$WORK/handvia_targets.txt") pads"
        xvfb-run -a python3 "$DRIVER_DIR/handvia.py" "$BOARD" "$WORK/plan_handvia.json" $(cat "$WORK/handvia_targets.txt") 2>&1 | tail -2 || log "handvia failed, continuing with routed board"
        if [ -s "$WORK/plan_handvia.json" ]; then
          xvfb-run -a python3 "$DRIVER_DIR/apply_plan.py" "$BOARD" "$WORK/plan_handvia.json" 2>&1 | tail -2
          merge_plan "$WORK/plan_handvia.json"
          stage_drc "handvia" || true
        fi
      fi
    fi
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
