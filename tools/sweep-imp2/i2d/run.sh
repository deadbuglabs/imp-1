#!/bin/bash
# Family i2d — case-first (outline/mounting locked before routing).
# The outline is the ruled Waveshare 29318 module outline, 61.00 x 92.44 mm,
# already the board outline. The case-first work is: (1) mounting holes per
# the 29318 hole pattern — pattern UNVERIFIED, so any hole placement this
# family adds carries that as an explicit assumption to verify, never as
# fact; (2) the 0.5 mm edge-keepout frame (targets the 9
# copper_edge_clearance traces at 0.22-0.49 mm from the edge).
# HONEST TRADE, carried from IMP-1 v17: a locked outline can FENCE the
# routing — v17's fence proofs showed exactly that failure mode. A
# case-first entry is only a win if the board routes inside the locked
# outline. If it cannot route, the result is reported as FENCED with the
# proof (DRC counts + which nets fence), never forced.
# TARGETED STAGE (not yet implemented — family worker job): mounting-hole
# placement (assumption-flagged) + edge-keepout frame as board geometry,
# then Freerouting for the remainder. Until that driver lands, this wrapper
# runs the shared Freerouting control stage only; the fr-mp3-edge variant
# is the probe of the edge-keepout idea (it verifies whether the keepout
# even reaches the DSN — the pass-2 attempt says it does not).
#   run.sh <variant> <board.kicad_pcb> <workdir>
set -u
VARIANT=$1; BOARD=$2; WORKDIR=$3
echo "family i2d (case-first): targeted outline/mounting stage not yet implemented; control FR stage only" >> "$WORKDIR/driver_notes.txt"
bash tools/sweep-imp2/fr/run.sh "$VARIANT" "$BOARD" "$WORKDIR"
