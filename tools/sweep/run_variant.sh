#!/bin/bash
# Run one sweep variant for one family.
#   run_variant.sh <family> <variant> <run_number> <run_id>
# Contract (see tools/sweep/README.md):
#   - Family board lives at hardware/pcb/main-v5/imp1_main_<family>.kicad_pcb
#     in the repo, with its name-matched .kicad_pro sibling. Absent board or
#     pro -> status "blocked", no routing attempted, no time burned.
#   - Family driver, if present, lives at tools/sweep/<family>/run.sh and is
#     called as: run.sh <variant> <board.kicad_pcb> <workdir>
#     It routes the board in place. Absent driver -> "blocked".
#   - This harness owns: baseline DRC + placement gate, final DRC, verdict
#     vs v15 (15/0/0), results JSON. It never fails the job on a blocked or
#     no-beat result; those are results, not errors. Hard toolchain failures
#     (pcbnew missing, DRC crash) exit non-zero.
set -u
FAMILY=$1; VARIANT=$2; RUN_NUMBER=$3; RUN_ID=$4
ROOT=$(pwd)
OUT=$ROOT/out
mkdir -p "$OUT"
BOARD_SRC=$ROOT/hardware/pcb/main-v5/imp1_main_${FAMILY}.kicad_pcb
PRO_SRC=$ROOT/hardware/pcb/main-v5/imp1_main_${FAMILY}.kicad_pro
RESULT=$OUT/${FAMILY}-${VARIANT}.json

finish() { # finish <status> <reason> [counts.json] [baseline.json]
  local status=$1 reason=$2 counts=${3:-} base=${4:-}
  local args=(--family "$FAMILY" --variant "$VARIANT" --run-number "$RUN_NUMBER" --run-id "$RUN_ID" --status "$status" --reason "$reason" --out "$RESULT")
  [ -n "$counts" ] && args+=(--counts "$counts")
  [ -n "$base" ] && args+=(--baseline "$base")
  [ -f "$OUT/board.kicad_pcb" ] && args+=(--board "$OUT/board.kicad_pcb")
  python3 tools/sweep/write_results.py "${args[@]}"
  echo "RESULT $status: $reason"
}

# --- 1. Family board present? ---
if [ ! -f "$BOARD_SRC" ]; then
  finish blocked "family board imp1_main_${FAMILY}.kicad_pcb not in repo yet (family worker has not landed it)"
  exit 0
fi
if [ ! -f "$PRO_SRC" ]; then
  finish blocked "family board present but name-matched .kicad_pro missing (DRC counts would be wrong without project settings)"
  exit 0
fi
cp "$BOARD_SRC" "$PRO_SRC" "$OUT/"
mv "$OUT/imp1_main_${FAMILY}.kicad_pcb" "$OUT/board.kicad_pcb"
mv "$OUT/imp1_main_${FAMILY}.kicad_pro" "$OUT/board.kicad_pro"

# --- 2. Baseline DRC + placement gate ---
xvfb-run -a python3 tools/sweep/drc.py "$OUT/board.kicad_pcb" "$OUT/baseline_drc.txt" || { finish blocked "baseline DRC crashed (toolchain)"; exit 1; }
python3 tools/sweep/parse_drc.py "$OUT/baseline_drc.txt" "$OUT/baseline_counts.json"
BASE_UNC=$(python3 -c "import json; print(json.load(open('$OUT/baseline_counts.json'))['unconnected'])")
BASE_SHORTS=$(python3 -c "import json; print(json.load(open('$OUT/baseline_counts.json'))['shorts'])")
BASE_CLR=$(python3 -c "import json; print(json.load(open('$OUT/baseline_counts.json'))['clearance'])")
echo "baseline: unconnected=$BASE_UNC shorts=$BASE_SHORTS clearance=$BASE_CLR"
if [ "$BASE_SHORTS" != "0" ]; then
  finish blocked "placement gate: baseline has $BASE_SHORTS shorts before any routing" "" "$OUT/baseline_counts.json"
  exit 0
fi
if [ "$BASE_CLR" != "0" ]; then
  finish blocked "placement gate: baseline has $BASE_CLR clearance before any routing" "" "$OUT/baseline_counts.json"
  exit 0
fi

# --- 3. Family driver present? ---
DRIVER=tools/sweep/${FAMILY}/run.sh
if [ ! -f "$DRIVER" ]; then
  finish blocked "family driver tools/sweep/${FAMILY}/run.sh not in repo yet (family worker has not landed it)" "" "$OUT/baseline_counts.json"
  exit 0
fi
chmod +x "$DRIVER" 2>/dev/null || true

# --- 4. Route (driver owns stages; it should call check_failfast.py
#         after each stage and stop early when it cannot beat v15) ---
bash "$DRIVER" "$VARIANT" "$OUT/board.kicad_pcb" "$OUT" || {
  finish blocked "family driver exited non-zero for variant $VARIANT" "" "$OUT/baseline_counts.json"
  exit 0
}

# --- 5. Final DRC + verdict ---
xvfb-run -a python3 tools/sweep/drc.py "$OUT/board.kicad_pcb" "$OUT/${FAMILY}-${VARIANT}_drc.txt" || { finish blocked "final DRC crashed (toolchain)"; exit 1; }
python3 tools/sweep/parse_drc.py "$OUT/${FAMILY}-${VARIANT}_drc.txt" "$OUT/${FAMILY}-${VARIANT}_counts.json"
UNC=$(python3 -c "import json; print(json.load(open('$OUT/${FAMILY}-${VARIANT}_counts.json'))['unconnected'])")
if [ "$UNC" -lt 15 ]; then
  # Double-check shorts/clearance are 0 before claiming a beat (verdict in
  # write_results.py enforces this too; this is the early log line).
  finish done "routed to $UNC unconnected (v15 is 15)" "$OUT/${FAMILY}-${VARIANT}_counts.json" "$OUT/baseline_counts.json"
else
  finish done "routed to $UNC unconnected, did not beat v15 (15/0/0)" "$OUT/${FAMILY}-${VARIANT}_counts.json" "$OUT/baseline_counts.json"
fi
