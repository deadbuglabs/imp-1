#!/bin/bash
# v18a cell driver — GitHub Actions port of the local v18a sweep supervisor.
#
# Usage: run_cell.sh <cell> <board.kicad_pcb> <workdir>
#   cell:    S01..S13 (see tools/sweep-v18a/README-v18a.md for the cell table)
#   board:   pristine v18a board (repo hardware/pcb/main-v5/imp1_main_v18a.kicad_pcb)
#   workdir: $WORKDIR — scratch, outputs and the sweep-results worktree live here
#
# Expected $WORKDIR layout (set up by the workflow):
#   $WORKDIR/board.kicad_pcb            working board (this script's copy)
#   $WORKDIR/pristine.kicad_pcb         untouched copy of the input board
#   $WORKDIR/cell-artifacts/            canonical state + .ses/plan artifacts
#     <CELL>-state.json                   schema imp1-sweep-v18a-state/1
#     <CELL>/ses/chunk_NNN.ses            one .ses per completed FR chunk
#     <CELL>/plans/plan_<PHASE>.json      one plan per completed A* phase
#   $WORKDIR/sweep-results/             checkout/worktree of sweep-results branch
#     results/v18a/<CELL>-state.json    mirror of the state file (pushed)
#     results/v18a/<CELL>/...           mirror of the artifacts (pushed)
#   $WORKDIR/out/<CELL>.json            final results JSON (imp1-sweep-result/1)
#
# Behaviour:
#   1. If a state file exists (worktree first, then local), rebuild the
#      checkpoint board with rebuild_state.py and VERIFY it: DRC the rebuilt
#      board and require unconnected == state's last_drc.unconnected, else
#      exit status blocked, reason "rebuild-mismatch". This verification IS
#      the resume proof — a board is never trusted from state alone.
#   2. Baseline DRC on the start board; shorts != 0 or clearance != 0 ->
#      blocked (a ripped board must be 146/0/0; anything else is not the
#      cell's start state). Baseline is recorded in the result.
#   3. FR cells (S01, S02, S03, S12-bulk): chunk loop. Each chunk exports a
#      fresh DSN from the current board, injects costs for S02/S03, runs
#      Freerouting -mp 1 (see DECISION below), imports the .ses into a FRESH
#      copy of the pristine board (never double-adds routes), DRCs, records.
#      Stops on unconnected == 0, stall >= 3, chunk cap, or 240 min elapsed.
#   4. A* cells (S04-S11, S13) and the S12 finish: phases in order from
#      phase_cmds.txt, skipping phases already in state.done_phases. Each
#      phase = route17.py -> validate plan JSON -> apply_plan.py -> DRC ->
#      check_failfast.py gate (STOP ends the cell as failfast).
#   5. S12 (after FR loop) and S13 (after phases): stitch17.py on the last
#      DRC report + apply + final DRC.
#   6. Writes the results JSON (tools/sweep/write_results.py, family v18a,
#      variant <CELL>) plus a "cell_state" object, to $WORKDIR/out/<CELL>.json,
#      and updates the state file in cell-artifacts/ + the worktree mirror.
#
# DECISION — chunking supersedes the single-run specs (recorded once, here
# and in README-v18a.md): the CELL_LOG specs say S01/S02 no -mp flag and
# S03 -mp 100, but the chunked driver runs EVERY FR chunk at -mp 1 (the
# sibling supervisor forces -mp 1 per chunk; a single -mp 100 run cannot
# resume). S03's "-mp 100" becomes a 100-chunk cap; S01/S02 run until
# done/stall (cap 60). One decision, recorded once.
#
# Exit codes: 0 whenever a results JSON was produced — blocked / no-beat /
# failfast are RESULTS, not failures. Non-zero ONLY if no result JSON could
# be produced at all (driver broke before the final write).
#
# No absolute paths. No locks, no sibling workers — one runner, one cell.
# Never touches the input board; all work happens on $WORKDIR copies.
set -u
CELL=${1:?cell S01..S13}; BOARD_IN=${2:?board.kicad_pcb}; WORK=${3:?workdir}
TOOL_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$TOOL_DIR/../.." && pwd)
SWEEP_DIR=$REPO_ROOT/tools/sweep
V18A_DIR=$REPO_ROOT/tools/sweep/v18a
JAR=$REPO_ROOT/tools/freerouting/freerouting.jar
RUN_NUMBER=${GITHUB_RUN_NUMBER:-0}
RUN_ID=${GITHUB_RUN_ID:-local}
START_S=$(date +%s)
ELAPSED_LIMIT=14400   # 240 min: stop starting new work, leave room for final DRC + push in the 300-min job
CHUNK_TIMEOUT=2400    # 40 min per FR chunk (supervise.sh value)
PHASE_TIMEOUT=3000    # 50 min per A* phase (run_astar_phase.sh value)
DRC_TIMEOUT=900
STALL_LIMIT=3
V15_UNC=15

WORK_BOARD=$WORK/board.kicad_pcb
PRISTINE=$WORK/pristine.kicad_pcb
LOCAL_ROOT=$WORK/cell-artifacts
STATE_LOCAL=$LOCAL_ROOT/$CELL-state.json
ART_LOCAL=$LOCAL_ROOT/$CELL
WT_ROOT=$WORK/sweep-results/results/v18a
STATE_WT=$WT_ROOT/$CELL-state.json
OUT_JSON=$WORK/out/$CELL.json
mkdir -p "$WORK/out" "$LOCAL_ROOT" "$ART_LOCAL/ses" "$ART_LOCAL/plans"

log() { echo "[run_cell $CELL] $*"; }
elapsed() { echo $(( $(date +%s) - START_S )); }
now_utc() { date -u +%Y-%m-%dT%H:%M:%SZ; }

# --- mutable run state (mirrored into the state file on every save) --------
CHUNK=0; STALL=0; BEST_UNC=9999; RESUME_FROM=pristine
DONE_PHASES=(); SES_FILES=(); PLAN_FILES=()
FINAL_STATUS=done; FINAL_REASON=""; FAILFAST=0; BLOCKED_REASON=""
LAST_DRC_TXT=""; LAST_COUNTS=""; CUR_UNC=""

copy_pro_sibling() { # $1 board path whose sibling to copy next to $2
  local src_base=${1%.kicad_pcb} dst_base=${2%.kicad_pcb}
  for ext in .kicad_pro .kicad_prl; do
    [ -f "$src_base$ext" ] && cp "$src_base$ext" "$dst_base$ext"
  done
}

# --- state file -------------------------------------------------------------
save_state() { # $1 = counts json path (last DRC); writes state + worktree mirror
  local counts_json=$1
  ST_CELL=$CELL ST_CHUNK=$CHUNK ST_STALL=$STALL \
  ST_PHASES="${DONE_PHASES[*]}" ST_SES="${SES_FILES[*]}" ST_PLANS="${PLAN_FILES[*]}" \
  ST_LAST="$counts_json" ST_RUN=$RUN_NUMBER ST_OUT=$STATE_LOCAL \
  python3 - <<'PY'
import json, os, datetime
def split(v):
    return v.split() if v.strip() else []
last = {}
lp = os.environ['ST_LAST']
if lp and os.path.exists(lp):
    last = json.load(open(lp))
st = {
    'schema': 'imp1-sweep-v18a-state/1',
    'cell': os.environ['ST_CELL'],
    'chunk': int(os.environ['ST_CHUNK']),
    'stall': int(os.environ['ST_STALL']),
    'done_phases': split(os.environ['ST_PHASES']),
    'last_drc': last,
    'ses_files': split(os.environ['ST_SES']),
    'plan_files': split(os.environ['ST_PLANS']),
    'updated_utc': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
    'run_number': int(os.environ['ST_RUN']),
}
with open(os.environ['ST_OUT'], 'w') as fh:
    json.dump(st, fh, indent=2)
PY
  if [ -d "$WT_ROOT" ]; then
    mkdir -p "$WT_ROOT/$CELL"
    cp "$STATE_LOCAL" "$STATE_WT"
    cp -r "$ART_LOCAL/." "$WT_ROOT/$CELL/" 2>/dev/null || true
  fi
}

load_state_file() { # $1 = state json path; sets STATE_* vars
  eval "$(python3 - "$1" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
print("STATE_CHUNK=%d" % s.get('chunk', 0))
print("STATE_STALL=%d" % s.get('stall', 0))
print("STATE_LAST_UNC=%s" % (s.get('last_drc') or {}).get('unconnected', ''))
print("STATE_PHASES='%s'" % ' '.join(s.get('done_phases', [])))
print("STATE_SES='%s'" % ' '.join(s.get('ses_files', [])))
print("STATE_PLANS='%s'" % ' '.join(s.get('plan_files', [])))
PY
)"
}

# --- DRC --------------------------------------------------------------------
# NOTE: run_drc sets globals (LAST_DRC_TXT, LAST_COUNTS, DRC_LINE) in the
# CALLER's shell — never capture it with $(...), which subshells and loses them.
run_drc() { # $1 board $2 tag; writes drc_<tag>.txt + counts_<tag>.json
  local board=$1 tag=$2
  timeout "$DRC_TIMEOUT" xvfb-run -a python3 "$SWEEP_DIR/drc.py" "$board" "$WORK/drc_$tag.txt" >/dev/null 2>&1 || { log "DRC failed at $tag"; return 1; }
  python3 "$SWEEP_DIR/parse_drc.py" "$WORK/drc_$tag.txt" "$WORK/counts_$tag.json" >/dev/null 2>&1 || { log "DRC parse failed at $tag"; return 1; }
  LAST_DRC_TXT=$WORK/drc_$tag.txt; LAST_COUNTS=$WORK/counts_$tag.json
  DRC_LINE=$(python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); print(c["unconnected"], c["shorts"], c["clearance"])' "$LAST_COUNTS")
}

# --- final result -----------------------------------------------------------
write_result() { # always tries to produce $OUT_JSON; returns 0 iff it exists
  local counts_arg="" board_arg=""
  [ -n "$LAST_COUNTS" ] && [ -f "$LAST_COUNTS" ] && counts_arg="--counts $LAST_COUNTS"
  [ -f "$WORK_BOARD" ] && board_arg="--board $WORK_BOARD"
  # shellcheck disable=SC2086
  python3 "$SWEEP_DIR/write_results.py" --family v18a --variant "$CELL" \
    --run-number "$RUN_NUMBER" --run-id "$RUN_ID" $counts_arg \
    --baseline "$WORK/counts_baseline.json" $board_arg \
    --status "$FINAL_STATUS" --reason "$FINAL_REASON" --out "$OUT_JSON" >/dev/null 2>&1 || true
  if [ ! -f "$OUT_JSON" ]; then
    log "write_results.py failed — hand-assembling minimal result JSON"
    python3 - "$OUT_JSON" <<PY || true
import json, datetime, sys
res = {'schema': 'imp1-sweep-result/1', 'run_number': int('$RUN_NUMBER'),
       'run_id': '$RUN_ID', 'family': 'v18a', 'variant': '$CELL',
       'status': '$FINAL_STATUS', 'reason': '$FINAL_REASON',
       'v15_reference': {'unconnected': 15, 'shorts': 0, 'clearance': 0},
       'verdict_vs_v15': 'n/a', 'drc': None, 'baseline_drc': None,
       'board_md5': None,
       'finished_utc': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}
json.dump(res, open(sys.argv[1], 'w'), indent=2)
PY
  fi
  if [ -f "$OUT_JSON" ]; then
    # Inject the cell_state object (write_results.py has no field for it).
    # Schema stays imp1-sweep-result/1; cell_state is the one extra field.
    python3 - "$OUT_JSON" <<PY
import json, sys
p = sys.argv[1]
res = json.load(open(p))
res['cell_state'] = {
    'chunks': int('$CHUNK'),
    'stall': int('$STALL'),
    'done_phases': '''$DONE_PHASES_STR'''.split(),
    'resume_from': '$RESUME_FROM',
    'checkpoint_ref': 'results/v18a/$CELL-state.json',
}
json.dump(res, open(p, 'w'), indent=2)
PY
    return 0
  fi
  return 1
}

finish() { # write result + final state save; exit 0 iff result exists
  DONE_PHASES_STR="${DONE_PHASES[*]}"
  export DONE_PHASES_STR
  if write_result; then
    [ -n "$LAST_COUNTS" ] && save_state "$LAST_COUNTS"
    log "result written: $OUT_JSON status=$FINAL_STATUS reason=$FINAL_REASON"
    exit 0
  fi
  log "FATAL: no result JSON could be produced"
  exit 1
}

# --- cell classification ----------------------------------------------------
case "$CELL" in
  S01|S02|S03) KIND=fr ;;
  S12)         KIND=fr_astar_stitch ;;
  S04|S05|S06|S07|S08|S09|S10|S11) KIND=astar ;;
  S13)         KIND=astar_stitch ;;
  *) log "unknown cell: $CELL"; FINAL_STATUS=blocked; FINAL_REASON="unknown-cell $CELL"; finish ;;
esac

# --- preflight --------------------------------------------------------------
[ -f "$BOARD_IN" ] || { log "input board missing: $BOARD_IN"; FINAL_STATUS=blocked; FINAL_REASON="input-board-missing"; finish; }
cp "$BOARD_IN" "$PRISTINE"; copy_pro_sibling "$BOARD_IN" "$PRISTINE"
cp "$PRISTINE" "$WORK_BOARD"; copy_pro_sibling "$PRISTINE" "$WORK_BOARD"
case "$KIND" in
  fr|fr_astar_stitch)
    [ -f "$JAR" ] || { log "freerouting jar missing: $JAR"; FINAL_STATUS=blocked; FINAL_REASON="freerouting-jar-missing"; finish; }
    command -v java >/dev/null 2>&1 || { log "java missing"; FINAL_STATUS=blocked; FINAL_REASON="java-missing"; finish; }
    ;;
  astar|astar_stitch)
    python3 -c "import numpy, PIL, scipy" 2>/dev/null || { log "blib deps missing (numpy/PIL/scipy)"; FINAL_STATUS=blocked; FINAL_REASON="blib-deps-missing"; finish; }
    ;;
esac
command -v xvfb-run >/dev/null 2>&1 || { log "xvfb-run missing"; FINAL_STATUS=blocked; FINAL_REASON="xvfb-missing"; finish; }

# --- resume: rebuild from state if one exists -------------------------------
STATE_SRC=""
[ -f "$STATE_WT" ] && STATE_SRC=$STATE_WT
[ -z "$STATE_SRC" ] && [ -f "$STATE_LOCAL" ] && STATE_SRC=$STATE_LOCAL
if [ -n "$STATE_SRC" ]; then
  log "resuming from state: $STATE_SRC"
  # Bring the committed artifacts local so rebuild + later chunks work on them.
  if [ "$STATE_SRC" = "$STATE_WT" ]; then
    cp "$STATE_WT" "$STATE_LOCAL"
    [ -d "$WT_ROOT/$CELL" ] && cp -r "$WT_ROOT/$CELL/." "$ART_LOCAL/" 2>/dev/null || true
  fi
  load_state_file "$STATE_SRC"
  CHUNK=${STATE_CHUNK:-0}; STALL=${STATE_STALL:-0}
  # shellcheck disable=SC2206
  DONE_PHASES=($STATE_PHASES); SES_FILES=($STATE_SES); PLAN_FILES=($STATE_PLANS)
  RESUME_FROM="state chunk $CHUNK"
  REPLAY_ROOT=$(dirname "$STATE_SRC")
  [ "$STATE_SRC" = "$STATE_WT" ] && REPLAY_ROOT=$WT_ROOT
  REBUILT=$WORK/rebuilt.kicad_pcb
  if timeout "$DRC_TIMEOUT" xvfb-run -a python3 "$TOOL_DIR/rebuild_state.py" "$STATE_SRC" "$PRISTINE" "$REBUILT" "$REPLAY_ROOT" > "$WORK/rebuild.log" 2>&1; then
    copy_pro_sibling "$PRISTINE" "$REBUILT"
    if run_drc "$REBUILT" "rebuild"; then
      set -- $DRC_LINE; REB_UNC=$1
      if [ "$REB_UNC" = "${STATE_LAST_UNC:-}" ]; then
        log "rebuild verified: unconnected $REB_UNC == state last_drc — resuming"
        mv "$REBUILT" "$WORK_BOARD"; copy_pro_sibling "$PRISTINE" "$WORK_BOARD"
        BEST_UNC=$REB_UNC
      else
        log "rebuild MISMATCH: rebuilt $REB_UNC != state ${STATE_LAST_UNC:-?}"
        FINAL_STATUS=blocked; FINAL_REASON="rebuild-mismatch rebuilt=$REB_UNC state=${STATE_LAST_UNC:-?}"; finish
      fi
    else
      log "DRC on rebuilt board failed"; FINAL_STATUS=blocked; FINAL_REASON="rebuild-drc-failed"; finish
    fi
  else
    log "rebuild_state.py failed (see $WORK/rebuild.log)"; FINAL_STATUS=blocked; FINAL_REASON="rebuild-failed"; finish
  fi
fi

# --- baseline DRC -----------------------------------------------------------
if run_drc "$WORK_BOARD" "baseline"; then
  set -- $DRC_LINE; BASE_UNC=$1; BASE_SHORTS=$2; BASE_CLR=$3
  log "baseline DRC: $BASE_UNC/$BASE_SHORTS/$BASE_CLR (unc/shorts/clearance)"
  [ "$BEST_UNC" = 9999 ] && BEST_UNC=$BASE_UNC
  if [ "$BASE_SHORTS" != 0 ] || [ "$BASE_CLR" != 0 ]; then
    FINAL_STATUS=blocked; FINAL_REASON="baseline-shorts-clearance unc=$BASE_UNC shorts=$BASE_SHORTS clr=$BASE_CLR"; finish
  fi
else
  FINAL_STATUS=blocked; FINAL_REASON="baseline-drc-failed"; finish
fi
PREV_COUNTS=$WORK/counts_baseline.json

# --- Freerouting chunk machinery --------------------------------------------
run_fr_once() { # $1 dsn $2 ses $3 log; 0 iff a non-empty .ses was produced
  local dsn=$1 ses=$2 lg=$3
  rm -f "$ses"
  timeout "$CHUNK_TIMEOUT" xvfb-run -a java -jar "$JAR" -de "$dsn" -do "$ses" -mp 1 > "$lg" 2>&1 &
  local pid=$! started=0
  for _ in $(seq 1 20); do
    sleep 15
    if grep -q "Starting auto-routing" "$lg" 2>/dev/null; then started=1; break; fi
    if grep -q "not in a Specctra DSN file format" "$lg" 2>/dev/null; then
      log "FR rejected the input format — killing hung run"; kill $pid 2>/dev/null; break
    fi
    kill -0 $pid 2>/dev/null || break
  done
  if [ "$started" = 0 ] && kill -0 $pid 2>/dev/null; then
    log "FR did not start routing within 300s — killing hung run"; kill $pid 2>/dev/null
  fi
  wait $pid; local rc=$?
  log "FR run rc=$rc ses=$(stat -c%s "$ses" 2>/dev/null || echo missing)"
  [ "$rc" = 0 ] && [ -s "$ses" ]
}

fr_chunk() { # $1 chunk number; 0 ok (DRC recorded, CUR_UNC set), 1 failed
  local n=$1 dsn=$WORK/chunk_$n.dsn ses=$WORK/chunk_$n.ses lg=$WORK/fr_c$n.log
  timeout 600 xvfb-run -a python3 "$SWEEP_DIR/export_dsn.py" "$WORK_BOARD" "$dsn" >/dev/null 2>&1 || { log "DSN export failed for chunk $n"; return 1; }
  case "$CELL" in
    S02) python3 "$TOOL_DIR/inject_autoroute.py" "$dsn" "$dsn" --via-costs 500 --plane-via-costs 50 || return 1 ;;
    S03) python3 "$TOOL_DIR/inject_autoroute.py" "$dsn" "$dsn" --start-ripup-costs 10 || return 1 ;;
  esac
  if ! run_fr_once "$dsn" "$ses" "$lg"; then
    log "chunk $n FR failed — ONE board-mediated retry from a freshly re-exported DSN"
    timeout 600 xvfb-run -a python3 "$SWEEP_DIR/export_dsn.py" "$WORK_BOARD" "$dsn" >/dev/null 2>&1 || return 1
    case "$CELL" in
      S02) python3 "$TOOL_DIR/inject_autoroute.py" "$dsn" "$dsn" --via-costs 500 --plane-via-costs 50 || return 1 ;;
      S03) python3 "$TOOL_DIR/inject_autoroute.py" "$dsn" "$dsn" --start-ripup-costs 10 || return 1 ;;
    esac
    run_fr_once "$dsn" "$ses" "$lg.retry" || { log "chunk $n retry also failed — stopping FR loop"; return 1; }
  fi
  # Import the .ses into a FRESH copy of the pristine board — never
  # double-adds routes, and a re-run of this chunk is idempotent.
  local tmpb=$WORK/.import-tmp.kicad_pcb
  cp "$PRISTINE" "$tmpb"
  timeout 600 xvfb-run -a python3 "$TOOL_DIR/import_ses.py" "$tmpb" "$ses" > "$WORK/import_c$n.log" 2>&1 || { log "import_ses failed for chunk $n"; return 1; }
  mv "$tmpb" "$WORK_BOARD"
  local ses_rel="$CELL/ses/chunk_$(printf %03d "$n").ses"
  cp "$ses" "$ART_LOCAL/ses/chunk_$(printf %03d "$n").ses"
  SES_FILES+=("$ses_rel")
  if run_drc "$WORK_BOARD" "fr_c$n"; then
    set -- $DRC_LINE; CUR_UNC=$1
    log "chunk $n DRC: $DRC_LINE (unc/shorts/clr)"
    return 0
  fi
  return 1
}

fr_loop() { # runs the FR chunk loop; sets FINAL_REASON
  local cap=60
  [ "$CELL" = S03 ] && cap=100
  while [ "$CHUNK" -lt "$cap" ]; do
    if [ "$(elapsed)" -gt "$ELAPSED_LIMIT" ]; then
      FINAL_REASON="time-budget chunks=$CHUNK unc=$CUR_UNC stall=$STALL"; return 0
    fi
    local n=$((CHUNK + 1))
    log "FR chunk $n/$cap (cell $CELL, -mp 1)"
    if ! fr_chunk "$n"; then
      FINAL_REASON="fr-chunk-failed chunks=$CHUNK unc=${CUR_UNC:-?} stall=$STALL"; return 0
    fi
    CHUNK=$n
    if [ "$CUR_UNC" -lt "$BEST_UNC" ]; then BEST_UNC=$CUR_UNC; STALL=0; else STALL=$((STALL + 1)); fi
    save_state "$LAST_COUNTS"; PREV_COUNTS=$LAST_COUNTS
    [ "$CUR_UNC" = 0 ] && { FINAL_REASON="fr-done chunks=$CHUNK unc=0"; return 0; }
    [ "$STALL" -ge "$STALL_LIMIT" ] && { FINAL_REASON="fr-stalled chunks=$CHUNK unc=$CUR_UNC stall=$STALL"; return 0; }
  done
  FINAL_REASON="fr-chunk-cap chunks=$CHUNK unc=$CUR_UNC stall=$STALL"
}

# --- A* phase machinery -----------------------------------------------------
phase_line() { # $1 cell $2 phase -> prints the raw phase_cmds.txt line
  grep -E "^$1 $2 " "$TOOL_DIR/phase_cmds.txt" | head -1
}

phase_order() { # prints the phase names for $CELL in run order
  case "$CELL" in
    S10) echo "E-exits A B C D" ;;
    S11) echo "E-exits C B A D" ;;
    S12) echo "A-finish B-finish C-finish D-finish" ;;
    *)   grep -E "^$CELL " "$TOOL_DIR/phase_cmds.txt" | awk '{print $2}' | tr '\n' ' ' ;;
  esac
}

phase_done() { # $1 phase -> 0 if in DONE_PHASES
  local p=$1 x; for x in "${DONE_PHASES[@]}"; do [ "$x" = "$p" ] && return 0; done; return 1
}

astar_phase() { # $1 phase; 0 continue, 1 stop (failfast or driver error)
  local phase=$1 line mf
  line=$(phase_line "$CELL" "$phase")
  [ -n "$line" ] || { log "no phase spec for $CELL $phase — skipping"; return 0; }
  set -- $line; mf=$3; shift 3  # $1=cell $2=phase $3=MF, rest = NET:w,... jobs
  local plan=$WORK/plan_$phase.json lg=$WORK/log_$phase.txt
  log "A* phase $phase MF=$mf nets: $*"
  if [ "$(elapsed)" -gt "$ELAPSED_LIMIT" ]; then
    FINAL_REASON="time-budget phases=${DONE_PHASES[*]} unc=${CUR_UNC:-?}"; return 1
  fi
  timeout "$PHASE_TIMEOUT" xvfb-run -a python3 "$V18A_DIR/route17.py" "$WORK_BOARD" "$plan" "$lg" "$mf" "$@" >> "$WORK/run_cell.log" 2>&1
  local rc=$?
  log "route17 $phase rc=$rc (124=timeout, plan checkpointed per link)"
  if [ -s "$plan" ] && python3 -m json.tool "$plan" >/dev/null 2>&1; then
    timeout 600 xvfb-run -a python3 "$V18A_DIR/apply_plan.py" "$WORK_BOARD" "$plan" >> "$WORK/run_cell.log" 2>&1
    log "apply_plan $phase rc=$?"
    cp "$plan" "$ART_LOCAL/plans/plan_$phase.json"
    PLAN_FILES+=("$CELL/plans/plan_$phase.json")
  else
    log "phase $phase produced no valid plan (all sealed or empty)"
  fi
  if run_drc "$WORK_BOARD" "$phase"; then
    set -- $DRC_LINE; CUR_UNC=$1
    log "phase $phase DRC: $DRC_LINE (unc/shorts/clr)"
  else
    log "DRC failed after phase $phase"; return 1
  fi
  DONE_PHASES+=("$phase")
  save_state "$LAST_COUNTS"
  if python3 "$SWEEP_DIR/check_failfast.py" "$PREV_COUNTS" "$LAST_COUNTS" >> "$WORK/run_cell.log" 2>&1; then
    PREV_COUNTS=$LAST_COUNTS; return 0
  fi
  log "FAIL-FAST STOP after phase $phase"
  FAILFAST=1; FINAL_STATUS=failfast
  FINAL_REASON="failfast after phase $phase unc=$CUR_UNC"
  return 1
}

astar_run() { # runs all phases for $CELL in order; 0 = ran to end, 1 = stopped early
  local phase
  for phase in $(phase_order); do
    phase_done "$phase" && { log "phase $phase already done — skipping"; continue; }
    astar_phase "$phase" || return 1
  done
  return 0
}

stitch_finish() { # stitch17.py on the last DRC report + apply + DRC
  local plan=$WORK/plan_stitch.json
  [ -f "$LAST_DRC_TXT" ] || { log "no DRC report for stitch — skipping"; return 0; }
  log "stitch finish on $LAST_DRC_TXT"
  timeout 600 xvfb-run -a python3 "$TOOL_DIR/stitch17.py" "$WORK_BOARD" "$LAST_DRC_TXT" "$plan" >> "$WORK/run_cell.log" 2>&1
  if [ -s "$plan" ] && python3 -c 'import json,sys; p=json.load(open(sys.argv[1])); sys.exit(0 if (p.get("add_tracks") or p.get("add_vias")) else 1)' "$plan"; then
    timeout 600 xvfb-run -a python3 "$V18A_DIR/apply_plan.py" "$WORK_BOARD" "$plan" >> "$WORK/run_cell.log" 2>&1
    cp "$plan" "$ART_LOCAL/plans/plan_stitch.json"
    PLAN_FILES+=("$CELL/plans/plan_stitch.json")
    if run_drc "$WORK_BOARD" "stitch"; then
      set -- $DRC_LINE; CUR_UNC=$1
      log "stitch DRC: $DRC_LINE (unc/shorts/clr)"
      save_state "$LAST_COUNTS"
    fi
  else
    log "stitch produced no tracks/vias — nothing to apply"
  fi
}

# --- main dispatch ----------------------------------------------------------
case "$KIND" in
  fr)
    fr_loop
    ;;
  astar)
    if astar_run; then
      FINAL_REASON="astar-complete phases=${DONE_PHASES[*]} unc=${CUR_UNC:-?}"
    elif [ "$FAILFAST" = 1 ]; then
      : # FINAL_STATUS/FINAL_REASON already set by astar_phase
    else
      FINAL_REASON="astar-stopped phases=${DONE_PHASES[*]} unc=${CUR_UNC:-?}"
    fi
    ;;
  fr_astar_stitch)
    fr_loop
    # S12: A* 0.02 finish in power-first order, then stitch — only if the
    # FR loop ended without a failfast/blocked state and time remains.
    if [ "$(elapsed)" -le "$ELAPSED_LIMIT" ]; then
      if astar_run; then
        FINAL_REASON="$FINAL_REASON + astar-finish-complete unc=${CUR_UNC:-?}"
      elif [ "$FAILFAST" = 1 ]; then
        : # failfast stands; stitch still runs (it can only close nets)
      else
        FINAL_REASON="$FINAL_REASON + astar-finish-stopped unc=${CUR_UNC:-?}"
      fi
      stitch_finish
      FINAL_REASON="$FINAL_REASON + stitch unc=${CUR_UNC:-?}"
    fi
    ;;
  astar_stitch)
    if astar_run; then
      FINAL_REASON="astar-complete phases=${DONE_PHASES[*]} unc=${CUR_UNC:-?}"
    elif [ "$FAILFAST" = 1 ]; then
      : # failfast stands; stitch still runs
    else
      FINAL_REASON="astar-stopped phases=${DONE_PHASES[*]} unc=${CUR_UNC:-?}"
    fi
    stitch_finish
    FINAL_REASON="$FINAL_REASON + stitch unc=${CUR_UNC:-?}"
    ;;
esac

# Final DRC is already the last stage DRC (LAST_COUNTS); the result's drc
# comes from it. If somehow no stage ran, fall back to the baseline counts.
[ -z "$LAST_COUNTS" ] && LAST_COUNTS=$WORK/counts_baseline.json
[ -z "$FINAL_REASON" ] && FINAL_REASON="complete unc=${CUR_UNC:-?}"
finish
