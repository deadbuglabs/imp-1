#!/bin/bash
# Family i2a — U7-first (TCA9535 expander fanout).
# Opens: U7 13 (worst ref, tied with U12); I2C_SCL 11 + I2C_SDA 4 opens ride
# the expander bus. U7 is QFN-24 4x4 P0.5 (ASSUMED-VERIFY pin numbering).
# TARGETED STAGE (not yet implemented — family worker job): hand escape of
# the U7 QFN field + an I2C bus spine (SCL/SDA) before the autorouter runs,
# then Freerouting for the remainder, with check_failfast_imp2.py after the
# escape stage. Until that driver lands, this wrapper runs the shared
# Freerouting control stage only and says so in the results JSON.
#   run.sh <variant> <board.kicad_pcb> <workdir>
set -u
VARIANT=$1; BOARD=$2; WORKDIR=$3
echo "family i2a (U7-first): targeted escape stage not yet implemented; control FR stage only" >> "$WORKDIR/driver_notes.txt"
bash tools/sweep-imp2/fr/run.sh "$VARIANT" "$BOARD" "$WORKDIR"
