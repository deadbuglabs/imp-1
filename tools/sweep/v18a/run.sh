#!/bin/bash
# v18a family driver — IMP-1 Main placement sweep (GitHub Actions).
# Contract (tools/sweep/README.md, "Family driver interface" + "A* driver
# snapshot contract (live feed)"):
#   run.sh <variant> <board.kicad_pcb> <workdir>
# Variants: astar-0.02, astar-0.05, astar-0.09,
#           freerouting-mp1, freerouting-mp3, freerouting-mp10.
# Routes the board IN PLACE, in stages. After each stage it writes a stage
# DRC (drc.py + parse_drc.py), publishes it to $WORKDIR/drc.json, and calls
# check_failfast.py — on STOP it stops routing and exits 0 so the harness
# still runs the final DRC and records a result. A non-zero exit means the
# driver itself broke (toolchain), and the harness reports blocked.
#
# A* snapshot feed: the cumulative plan goes to $SWEEP_PLAN_PATH (default
# $WORKDIR/plan.json), checkpointed after every link by route17.py and
# merged per stage (atomic replace). progress.jsonl gets a start event then
# one line per finished net. Stage DRCs go to $WORKDIR/drc.json.
# Freerouting variants write none of the snapshot files (no watcher).
#
# No absolute paths, no locks, no sibling workers — one runner, one job.
# Stage time budget: route17.py calls are capped (timeout) and no new stage
# starts after 70 min, so the harness still has room for its final DRC
# inside the 90-min job cap.
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

log() { echo "[v18a $VARIANT] $*"; }

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

# --- phase net lists (from v18a ROUTING_LOG.md) ---------------------------
# widths: power 0.5,0.4 / signals 0.25,0.2 (family evidence: plan_A.json)
PHASE_A="BOOST_SW:0.5,0.4 SYS_5V:0.5,0.4 BAT_CELL:0.5,0.4 CELL_POS:0.5,0.4 CELL_NEG:0.5,0.4 BOOST_5V:0.5,0.4 BOOST_FB:0.5,0.4 A9_PWR:0.5,0.4 A9_GATE:0.5,0.4 A9_5V:0.5,0.4 VBUS_5V:0.5,0.4"
PHASE_B="RF_P:0.25,0.2 RF_N:0.25,0.2 BAL_A:0.25,0.2 BAL_B:0.25,0.2 BAL_C:0.25,0.2 BAL_D:0.25,0.2 BAL_E:0.25,0.2 BAL_BDC:0.25,0.2 RF_ANT:0.25,0.2 CC_DCOUPL:0.25,0.2 CC_XOSC_Q1:0.25,0.2 CC_XOSC_Q2:0.25,0.2 CC_RBIAS:0.25,0.2 SPI_SCLK:0.25,0.2 SPI_MOSI:0.25,0.2 SPI_MISO:0.25,0.2 CC_CS:0.25,0.2"
PHASE_C="FLASH_CS:0.25,0.2 FLASH_WP:0.25,0.2 DISP_CS:0.25,0.2 DISP_DC:0.25,0.2 DISP_RST:0.25,0.2 DISP_BL:0.25,0.2 CHG_ISET:0.25,0.2 CHG_ILIM:0.25,0.2 CHG_STAT:0.25,0.2 CHG_TMR:0.25,0.2 CHG_TS:0.25,0.2 ENC_B:0.25,0.2 BZR_OUT:0.25,0.2 FLASHLED_OUT:0.25,0.2 IR_RX_OUT:0.25,0.2 IR_TX:0.25,0.2 ST25_CS:0.25,0.2 ST25_IRQ:0.25,0.2 USB_DP:0.25,0.2 USB_DN:0.25,0.2 CC1:0.25,0.2 CC2:0.25,0.2"
PHASE_D="GND:0.5,0.4 V3_3:0.5,0.4"

case "$VARIANT" in
  astar-0.02|astar-0.05|astar-0.09)
    MF=${VARIANT#astar-}
    export SWEEP_CONGESTION_WEIGHT=${SWEEP_CONGESTION_WEIGHT:-1.5}
    # blib needs numpy/PIL/scipy; the workflow installs kicad only, so fetch
    # them here if missing (apt, same lists the workflow already updated).
    python3 -c "import numpy, PIL, scipy, wx, pcbnew" 2>/dev/null || {
      log "installing python3-numpy python3-pil python3-scipy for blib"
      sudo apt-get install -y -qq python3-numpy python3-pil python3-scipy || { log "DEP FAIL: cannot install blib deps"; exit 1; }
      python3 -c "import numpy, PIL, scipy, wx, pcbnew" 2>/dev/null || { log "DEP FAIL: blib deps still unimportable"; exit 1; }
    }
    # snapshot feed: start event (52 nets across A+B+C+D), empty cumulative plan
    NETS_TOTAL=$(echo "$PHASE_A $PHASE_B $PHASE_C $PHASE_D" | wc -w)
    echo "{\"event\": \"start\", \"nets_total\": $NETS_TOTAL}" > "$PROGRESS"
    echo '{"codes": {}, "add_tracks": [], "add_vias": [], "delete": [], "delete_vias": []}' > "$PLAN_CUM"
    PREV="$WORK/baseline_counts.json"
    [ -f "$PREV" ] || { log "no baseline counts at $PREV; using empty prev"; echo '{"unconnected": 999, "shorts": 0, "clearance": 0}' > "$PREV"; }
    for stage in A B C D; do
      case $stage in
        A) jobs="$PHASE_A";; B) jobs="$PHASE_B";; C) jobs="$PHASE_C";; D) jobs="$PHASE_D";;
      esac
      astar_stage "$stage" "$MF" $jobs || { log "stage $stage driver error, stopping"; break; }
      if gate "$PREV" "$WORK/counts_$stage.json"; then
        PREV="$WORK/counts_$stage.json"
      else
        log "FAIL-FAST STOP after stage $stage"
        break
      fi
    done
    # Stage D pour assist: targeted handvia on GND/V3_3 pads still unconnected
    # per the last stage DRC (cheap ring search; family Phase D method).
    if [ "$(elapsed)" -lt "$BUDGET_S" ] && [ -f "$WORK/drc_D.txt" ]; then
      python3 - "$WORK/drc_D.txt" "$WORK/handvia_targets.txt" <<'PY'
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
