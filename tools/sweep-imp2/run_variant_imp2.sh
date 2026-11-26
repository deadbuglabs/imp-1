#!/bin/bash
# Run one IMP-2 sweep variant for one family.
#   run_variant_imp2.sh <family> <variant> <run_number> <run_id>
# Adapted from tools/sweep/run_variant.sh (IMP-1). Differences, all
# deliberate — see tools/sweep-imp2/README-imp2.md:
#   - TWO boards, like IMP-1 (v15 banked reference vs v18a family
#     placements):
#       * Banked reference (verdicts + toolchain smoke ONLY):
#         hardware/pcb/imp2/imp2_layout.kicad_pcb — the pass-2 mp3 keeper,
#         134/0/12. NEVER routed over: routing it again stacks a second
#         route on the first (proven locally 2026-11-25: FR mp1 on it gave
#         132 with 179 holes_co_located duplicate vias).
#       * Family board (what jobs actually route):
#         hardware/pcb/imp2/imp2_placement.kicad_pcb — the pass-2 placement
#         rebuilt from stage2 + the verbatim pass2_zones.py GND-zone recipe
#         (3 zones, 0.25 mm clearance). 0 tracks, 0 vias, 143 footprints.
#         Verified baseline 283 unconnected / 0 shorts / 12 clearance,
#         total 391 — exactly the log's placement baseline. All families
#         share it in v1; families differ by DRIVER, not by board.
#   - Reference counts for verdicts 134/0/12 (the banked best). The 12
#     clearance are FENCED (U12 0.4 mm-pitch pads at 0.15 mm vs the 0.2 mm
#     rule). The placement gate therefore allows clearance == 12 and blocks
#     only growth above 12 — IMP-1's gate (clearance > 0 blocks) would block
#     every IMP-2 job before routing. The family board's baseline is NOT
#     checked against 134 (it is a placement: v1 baseline is 283); the
#     toolchain job checks the banked 134 board instead.
#   - Verdict vs 134 (write_results_imp2.py, schema imp2-sweep-result/1).
#     Finish line is DRC-clean (0/0/0); beating 134 is progress, not a pass.
#   - NO snapshot watcher: the v1 matrix is Freerouting-only and
#     Freerouting is a black box — no mid-route plan checkpoints exist to
#     watch (see README-imp2.md "Live snapshots"). astar-* variants are not
#     in the v1 matrix; if an A* driver is ever ported to IMP-2, a watcher
#     port (imp2 snapshot schema + results/imp2/ paths) lands with it.
#   - This harness owns: baseline DRC + placement gate, final DRC, verdict,
#     results JSON. It never fails the job on a blocked or no-beat result;
#     those are results, not errors. Hard toolchain failures (pcbnew
#     missing, DRC crash) exit non-zero.
set -u
FAMILY=$1; VARIANT=$2; RUN_NUMBER=$3; RUN_ID=$4
ROOT=$(pwd)
OUT=$ROOT/out
mkdir -p "$OUT"
BOARD_SRC=$ROOT/hardware/pcb/imp2/imp2_placement.kicad_pcb
PRO_SRC=$ROOT/hardware/pcb/imp2/imp2_placement.kicad_pro
RESULT=$OUT/${FAMILY}-${VARIANT}.json
REF_UNC=134; REF_SHORTS=0; REF_CLR=12

finish() { # finish <status> <reason> [counts.json] [baseline.json]
  local status=$1 reason=$2 counts=${3:-} base=${4:-}
  if [ -f "$OUT/driver_notes.txt" ]; then
    reason="$reason [driver: $(tr '\n' ' ' < "$OUT/driver_notes.txt" | sed 's/  */ /g; s/ *$//')]"
  fi
  local args=(--family "$FAMILY" --variant "$VARIANT" --run-number "$RUN_NUMBER" --run-id "$RUN_ID" --status "$status" --reason "$reason" --out "$RESULT")
  [ -n "$counts" ] && args+=(--counts "$counts")
  [ -n "$base" ] && args+=(--baseline "$base")
  [ -f "$OUT/board.kicad_pcb" ] && args+=(--board "$OUT/board.kicad_pcb")
  python3 tools/sweep-imp2/write_results_imp2.py "${args[@]}"
  echo "RESULT $status: $reason"
}

# --- 1. Reference board present? ---
if [ ! -f "$BOARD_SRC" ]; then
  finish blocked "family board hardware/pcb/imp2/imp2_placement.kicad_pcb not in repo yet"
  exit 0
fi
if [ ! -f "$PRO_SRC" ]; then
  finish blocked "family board present but name-matched .kicad_pro missing (DRC counts would be wrong without project settings)"
  exit 0
fi
cp "$BOARD_SRC" "$PRO_SRC" "$OUT/"
mv "$OUT/imp2_placement.kicad_pcb" "$OUT/board.kicad_pcb"
mv "$OUT/imp2_placement.kicad_pro" "$OUT/board.kicad_pro"

# --- 2. Baseline DRC + placement gate (placement baseline, v1: 283/0/12) ---
xvfb-run -a python3 tools/sweep-imp2/drc.py "$OUT/board.kicad_pcb" "$OUT/baseline_drc.txt" || { finish blocked "baseline DRC crashed (toolchain)"; exit 1; }
python3 tools/sweep-imp2/parse_drc.py "$OUT/baseline_drc.txt" "$OUT/baseline_counts.json"
BASE_UNC=$(python3 -c "import json; print(json.load(open('$OUT/baseline_counts.json'))['unconnected'])")
BASE_SHORTS=$(python3 -c "import json; print(json.load(open('$OUT/baseline_counts.json'))['shorts'])")
BASE_CLR=$(python3 -c "import json; print(json.load(open('$OUT/baseline_counts.json'))['clearance'])")
echo "baseline: unconnected=$BASE_UNC shorts=$BASE_SHORTS clearance=$BASE_CLR (v1 placement baseline 283/0/12; reference to beat 134/0/12)"
if [ "$BASE_SHORTS" != "0" ]; then
  finish blocked "placement gate: baseline has $BASE_SHORTS shorts before any routing" "" "$OUT/baseline_counts.json"
  exit 0
fi
if [ "$BASE_CLR" -gt "$REF_CLR" ]; then
  finish blocked "placement gate: baseline clearance $BASE_CLR exceeds fenced 12 before any routing" "" "$OUT/baseline_counts.json"
  exit 0
fi

# --- 3. Family driver present? ---
DRIVER=tools/sweep-imp2/${FAMILY}/run.sh
if [ ! -f "$DRIVER" ]; then
  finish blocked "family driver tools/sweep-imp2/${FAMILY}/run.sh not in repo yet (family worker has not landed it)" "" "$OUT/baseline_counts.json"
  exit 0
fi
chmod +x "$DRIVER" 2>/dev/null || true

# --- 4. Route (driver owns stages; it should call check_failfast_imp2.py
#         after each stage and stop early when it cannot beat 134) ---
bash "$DRIVER" "$VARIANT" "$OUT/board.kicad_pcb" "$OUT"
DRIVER_RC=$?
if [ "$DRIVER_RC" != "0" ]; then
  finish blocked "family driver exited non-zero for variant $VARIANT" "" "$OUT/baseline_counts.json"
  exit 0
fi

# --- 5. Final DRC + verdict ---
xvfb-run -a python3 tools/sweep-imp2/drc.py "$OUT/board.kicad_pcb" "$OUT/${FAMILY}-${VARIANT}_drc.txt" || { finish blocked "final DRC crashed (toolchain)"; exit 1; }
python3 tools/sweep-imp2/parse_drc.py "$OUT/${FAMILY}-${VARIANT}_drc.txt" "$OUT/${FAMILY}-${VARIANT}_counts.json"
UNC=$(python3 -c "import json; print(json.load(open('$OUT/${FAMILY}-${VARIANT}_counts.json'))['unconnected'])")
CLR=$(python3 -c "import json; print(json.load(open('$OUT/${FAMILY}-${VARIANT}_counts.json'))['clearance'])")
if [ "$UNC" -lt "$REF_UNC" ] && [ "$CLR" -le "$REF_CLR" ]; then
  finish done "routed to $UNC unconnected, clearance $CLR (reference is 134/0/12)" "$OUT/${FAMILY}-${VARIANT}_counts.json" "$OUT/baseline_counts.json"
else
  finish done "routed to $UNC unconnected, clearance $CLR, did not beat reference (134/0/12)" "$OUT/${FAMILY}-${VARIANT}_counts.json" "$OUT/baseline_counts.json"
fi
