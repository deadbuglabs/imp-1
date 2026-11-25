#!/bin/bash
# Shared Freerouting driver for IMP-2 sweep families.
#   fr/run.sh <variant> <board.kicad_pcb> <workdir>
# Called by each family wrapper (tools/sweep-imp2/<family>/run.sh). Routes
# the board in place. Stages:
#   1. Export DSN (export_dsn.py). For fr-mp3-edge, an edge-keepout frame
#      is attempted first and the DSN keepout count is verified before/after.
#   2. For fr-mp3-finevia, the DSN via padstack is narrowed 0.6/0.3 -> 0.5/0.3
#      (diameter only; drill stays 0.3) and the edit is verified by count.
#   3. Freerouting 1.9.0 (jar at tools/freerouting/freerouting.jar, size
#      5044336) under $DISPLAY. -mp N from the variant. Batch mode writes
#      the .ses only at the end; -mp 10 is NOT offered (killed at 21 min
#      with no output in the pass-2 log).
#   4. Import the SES with import_ses_imp2.py (direct SES parse — proven
#      for this board; stock pcbnew.ImportSpecctraSES needs the GUI board).
#   5. Stage DRC + check_failfast_imp2.py vs the baseline counts; STOP is
#      logged (single-stage driver: nothing further to stop, harness DRC
#      still rules). Notes for the results JSON go to $WORKDIR/driver_notes.txt.
# Exit non-zero only on toolchain failure (missing jar/SES, DSN edit or
# keepout verification failure where the variant claims the edit happened).
# A no-op edge keepout is NOT a failure: it is recorded in driver_notes.txt
# and the run proceeds as plain mp3 — the result proves the no-op.
set -u
VARIANT=$1; BOARD=$2; WORKDIR=$3
ROOT=$(pwd)
SWEEP=tools/sweep-imp2
JAR=$ROOT/tools/freerouting/freerouting.jar
JAR_SIZE=5044336
note() { echo "$1" >> "$WORKDIR/driver_notes.txt"; }

case "$VARIANT" in
  fr-mp1) MP=1 ;;
  fr-mp3) MP=3 ;;
  fr-mp3-finevia) MP=3 ;;
  fr-mp3-edge) MP=3 ;;
  *) echo "unknown variant: $VARIANT"; exit 1 ;;
esac

[ -f "$JAR" ] || { echo "freerouting jar missing: $JAR"; exit 1; }
[ "$(stat -c%s "$JAR")" = "$JAR_SIZE" ] || { echo "freerouting jar size mismatch"; exit 1; }
[ -n "${DISPLAY:-}" ] || { echo "DISPLAY not set (harness must start Xvfb)"; exit 1; }

DSN=$WORKDIR/board.dsn
SES=$WORKDIR/board.ses

# --- 1. Edge keepout attempt (fr-mp3-edge only) ---
if [ "$VARIANT" = "fr-mp3-edge" ]; then
  xvfb-run -a python3 $SWEEP/export_dsn.py "$BOARD" "$WORKDIR/edge_before.dsn" || exit 1
  BEFORE=$(grep -c '(keepout' "$WORKDIR/edge_before.dsn")
  xvfb-run -a python3 - "$BOARD" <<'PY' || exit 1
import pcbnew, sys
BW, BH, M = 61.0, 92.44, 0.5
b = pcbnew.LoadBoard(sys.argv[1])
strips = [(0,0,BW,M), (0,BH-M,BW,BH), (0,0,M,BH), (BW-M,0,BW,BH)]
n = 0
for layer in ['F.Cu','In1.Cu','In2.Cu','B.Cu']:
    lid = b.GetLayerID(layer)
    for (x1,y1,x2,y2) in strips:
        z = pcbnew.ZONE(b)
        z.SetLayer(lid)
        z.SetDoNotAllowTracks(True); z.SetDoNotAllowVias(True)
        z.SetDoNotAllowPads(False); z.SetDoNotAllowFootprints(False)
        z.SetDoNotAllowCopperPour(True)
        z.SetLocalClearance(pcbnew.FromMM(0))
        o = z.Outline(); o.NewOutline()
        for (x,y) in [(x1,y1),(x2,y1),(x2,y2),(x1,y2)]:
            o.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
        b.Add(z); n += 1
pcbnew.SaveBoard(sys.argv[1], b)
print('keepout strips added:', n, flush=True)
PY
  xvfb-run -a python3 $SWEEP/export_dsn.py "$BOARD" "$DSN" || exit 1
  AFTER=$(grep -c '(keepout' "$DSN")
  if [ "$AFTER" = "$BEFORE" ]; then
    note "edge-keepout did NOT propagate to DSN (keepouts $BEFORE before and after); ran as plain mp3 — targets 9 copper_edge_clearance, unproven."
  else
    note "edge-keepout propagated to DSN (keepouts $BEFORE -> $AFTER)."
  fi
else
  xvfb-run -a python3 $SWEEP/export_dsn.py "$BOARD" "$DSN" || exit 1
fi

# --- 2. Fine-via DSN edit (fr-mp3-finevia only): 0.6 -> 0.5 mm dia, drill 0.3 ---
if [ "$VARIANT" = "fr-mp3-finevia" ]; then
  EXPECTED=$(grep -c 'Via\[0-3\]_600:300_um' "$DSN")
  python3 - "$DSN" <<'PY' || exit 1
import re, sys
p = sys.argv[1]
s = open(p).read()
s = s.replace('Via[0-3]_600:300_um', 'Via[0-3]_500:300_um')
# Narrow only the via padstack circles (600 -> 500 inside that padstack block)
def narrow(m):
    return m.group(0).replace('(circle F.Cu 600)', '(circle F.Cu 500)') \
                     .replace('(circle In1.Cu 600)', '(circle In1.Cu 500)') \
                     .replace('(circle In2.Cu 600)', '(circle In2.Cu 500)') \
                     .replace('(circle B.Cu 600)', '(circle B.Cu 500)')
s = re.sub(r'\(padstack "Via\[0-3\]_500:300_um".*?\(attach off\)\s*\)', narrow, s, flags=re.S)
open(p, 'w').write(s)
PY
  GOT=$(grep -c 'Via\[0-3\]_500:300_um' "$DSN")
  LEFT=$(grep -c 'Via\[0-3\]_600:300_um' "$DSN" || true)
  if [ "$GOT" != "$EXPECTED" ] || [ "$LEFT" != "0" ]; then
    echo "finevia DSN edit failed: renamed $GOT/$EXPECTED, $LEFT old refs left"; exit 1
  fi
  grep -A6 '(padstack "Via\[0-3\]_500:300_um"' "$DSN" | grep -q '(circle F.Cu 500)' || { echo "finevia padstack circles not narrowed"; exit 1; }
  note "finevia: via dia 0.6 -> 0.5 mm, drill stays 0.3 mm ($GOT refs renamed; annular ring exactly at 0.1 mm pro minimum — DRC judges)."
fi

# --- 3. Freerouting ---
rm -f "$SES"
TIMEOUT=600; [ "$MP" = "3" ] && TIMEOUT=1500
timeout "$TIMEOUT" java -jar "$JAR" -de "$DSN" -do "$SES" -mp "$MP" || { echo "freerouting failed or timed out (mp=$MP)"; exit 1; }
[ -s "$SES" ] || { echo "freerouting produced no .ses"; exit 1; }
echo "freerouting mp=$MP done: $(stat -c%s "$SES") bytes ses"

# --- 4. Import SES back into the board ---
xvfb-run -a python3 $SWEEP/import_ses_imp2.py "$BOARD" "$SES" || exit 1

# --- 5. Stage DRC + fail-fast vs baseline ---
xvfb-run -a python3 $SWEEP/drc.py "$BOARD" "$WORKDIR/stage_drc.txt" || exit 1
python3 $SWEEP/parse_drc.py "$WORKDIR/stage_drc.txt" "$WORKDIR/stage_counts.json" || exit 1
python3 $SWEEP/check_failfast_imp2.py "$WORKDIR/baseline_counts.json" "$WORKDIR/stage_counts.json" || echo "failfast STOP logged (single-stage driver; harness final DRC rules)"
exit 0
