#!/bin/bash
# Family i2b — U12-field-first (FUSB302B DRP/PD controller).
# Opens: U12 13 (tied worst ref); PD_INT_N 4; CC1/CC2. The 12 fenced
# clearance violations ALL live in this field (U12 0.4 mm-pitch pads at
# 0.15 mm vs the 0.2 mm rule). U12 pin numbering is ASSUMED-VERIFY.
# TARGETED STAGE (not yet implemented — family worker job): fine-via
# (0.5/0.25) escape fanout for the U12 field + a pin-number verification
# gate before any routing, then Freerouting for the remainder, with
# check_failfast_imp2.py after the escape stage. Until that driver lands,
# this wrapper runs the shared Freerouting control stage only and says so
# in the results JSON. (The fr-mp3-finevia variant is the probe of the
# fine-via escape idea at the whole-board level.)
#   run.sh <variant> <board.kicad_pcb> <workdir>
set -u
VARIANT=$1; BOARD=$2; WORKDIR=$3
echo "family i2b (U12-field-first): targeted escape stage not yet implemented; control FR stage only" >> "$WORKDIR/driver_notes.txt"
bash tools/sweep-imp2/fr/run.sh "$VARIANT" "$BOARD" "$WORKDIR"
