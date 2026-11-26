#!/usr/bin/env python3
"""Rebuild a v18a cell checkpoint board from state — pure replay, no routing.

Usage:
  rebuild_state.py <cell-state.json> <pristine-board> <out-board> [artifacts-root]

State schema: imp1-sweep-v18a-state/1. Replays, in order:
  1. state["ses_files"]   — each .ses imported into the board copy via
                             tools/sweep-v18a/import_ses.py (manual SES parse)
  2. state["plan_files"]  — each plan JSON applied via
                             tools/sweep/v18a/apply_plan.py

ses_files / plan_files entries are paths RELATIVE to artifacts-root
(default: the directory holding the state JSON). In the Actions layout the
artifacts root is $WORKDIR/sweep-results/results/v18a (state file at
results/v18a/<CELL>-state.json, artifacts at results/v18a/<CELL>/...).

The pristine board is copied to out-board first (with its .kicad_pro
sibling, which DRC needs for correct counts); every replay step works on
that copy, so a re-run is idempotent. Prints the rebuilt board's
unconnected-pad count (pcbnew connectivity) as the last line:
  rebuilt_unconnected=<N>
run_cell.sh DRCs the rebuilt board and requires that number to equal
state["last_drc"]["unconnected"] — that check IS the resume proof.

Must run under xvfb-run (pcbnew needs X); the replayed tools are invoked
as subprocesses inheriting DISPLAY.
"""
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', 'v18a')))


def _path(entry):
    if isinstance(entry, dict):
        return entry.get('path') or entry.get('file')
    return entry


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    state_path, pristine, out_board = sys.argv[1], sys.argv[2], sys.argv[3]
    root = sys.argv[4] if len(sys.argv) > 4 else os.path.dirname(os.path.abspath(state_path))
    state = json.load(open(state_path))
    if state.get('schema') != 'imp1-sweep-v18a-state/1':
        print('ERROR: unexpected state schema: %r' % state.get('schema'))
        return 2

    shutil.copyfile(pristine, out_board)
    for sib in ('.kicad_pro', '.kicad_prl'):
        src = os.path.splitext(pristine)[0] + sib
        if os.path.exists(src):
            shutil.copyfile(src, os.path.splitext(out_board)[0] + sib)

    import_ses = os.path.join(HERE, 'import_ses.py')
    apply_plan = os.path.normpath(os.path.join(HERE, '..', 'v18a', 'apply_plan.py'))

    for entry in state.get('ses_files', []):
        ses = os.path.join(root, _path(entry))
        if not os.path.exists(ses):
            print('ERROR: missing ses artifact: %s' % ses)
            return 2
        print('replay ses: %s' % ses, flush=True)
        r = subprocess.run([sys.executable, import_ses, out_board, ses])
        if r.returncode != 0:
            print('ERROR: import_ses failed on %s' % ses)
            return 2

    for entry in state.get('plan_files', []):
        plan = os.path.join(root, _path(entry))
        if not os.path.exists(plan):
            print('ERROR: missing plan artifact: %s' % plan)
            return 2
        print('replay plan: %s' % plan, flush=True)
        r = subprocess.run([sys.executable, apply_plan, out_board, plan])
        if r.returncode != 0:
            print('ERROR: apply_plan failed on %s' % plan)
            return 2

    import blib
    b = blib.load(out_board)
    b.BuildConnectivity()
    unc = b.GetConnectivity().GetUnconnectedCount(True)
    print('rebuilt_unconnected=%d' % unc)
    return 0


if __name__ == '__main__':
    sys.exit(main())
