#!/bin/bash
# Family i2c — power-first (power/ground spine before signals).
# Opens: GND 18 + V3_3 18 + VBUS_5V 9 = 45 of 134 (34%). Zone refill after
# routing DISCONNECTED GND pads in the pass-2 log (starved_thermal 24 -> 44
# after the -mp 1 import) — the planes need repair, not more signal passes.
# TARGETED STAGE (not yet implemented — family worker job): GND/V3_3 plane
# + VBUS_5V spine routed first (zones refilled and connectivity-checked
# before any signal routing), then Freerouting for the remainder, with
# check_failfast_imp2.py after the power stage. Until that driver lands,
# this wrapper runs the shared Freerouting control stage only and says so
# in the results JSON.
#   run.sh <variant> <board.kicad_pcb> <workdir>
set -u
VARIANT=$1; BOARD=$2; WORKDIR=$3
echo "family i2c (power-first): targeted power stage not yet implemented; control FR stage only" >> "$WORKDIR/driver_notes.txt"
bash tools/sweep-imp2/fr/run.sh "$VARIANT" "$BOARD" "$WORKDIR"
