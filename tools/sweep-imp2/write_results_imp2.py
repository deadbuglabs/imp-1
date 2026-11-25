"""Assemble one IMP-2 sweep job's results JSON.

Usage:
  python3 write_results_imp2.py --family i2a --variant fr-mp3 \
    --run-number 12 --run-id 123456789 --counts out/counts.json \
    --board out/board.kicad_pcb --status done|blocked|failfast|toolchain-ok \
    --reason '...' --out out/i2a-fr-mp3.json [--baseline out/base_counts.json]

Reference: IMP-2 layout pass 2, 134 unconnected / 0 shorts / 12 clearance
(the 12 are FENCED: U12's own 0.4 mm-pitch pads at 0.15 mm vs the 0.2 mm
Default netclass — footprint-vs-rule mismatch, within standard 4-layer fab
capability). Verdict vs the 134/0/12 reference:
  beat    unconnected < 134 and shorts == 0 and clearance <= 12
  tie     unconnected == 134 and shorts == 0 and clearance <= 12
  no-beat anything else (or no DRC at all -> n/a)
Clearance GROWTH above the fenced 12 is always no-beat, even if the
unconnected count drops. The finish line is DRC-clean (0/0/0); beating the
reference is progress toward it, not a pass. No fab, no orders, no gate PASS.
Schema is imp2-sweep-result/1 — namespaced apart from imp1-sweep-result/1.
"""
import argparse, datetime, hashlib, json, os, sys

REF = {'unconnected': 134, 'shorts': 0, 'clearance': 12}

def md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def verdict(drc):
    if drc is None:
        return 'n/a'
    u = drc.get('unconnected')
    if u is None:
        return 'n/a'
    if drc.get('shorts', 0) != 0 or drc.get('clearance', 0) > REF['clearance']:
        return 'no-beat'
    if u < REF['unconnected']:
        return 'beat'
    if u == REF['unconnected']:
        return 'tie'
    return 'no-beat'

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--family', required=True)
    ap.add_argument('--variant', required=True)
    ap.add_argument('--run-number', required=True)
    ap.add_argument('--run-id', required=True)
    ap.add_argument('--counts', required=False, default=None)
    ap.add_argument('--baseline', required=False, default=None)
    ap.add_argument('--board', required=False, default=None)
    ap.add_argument('--status', required=True)
    ap.add_argument('--reason', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    drc = json.load(open(a.counts)) if a.counts and os.path.exists(a.counts) else None
    base = json.load(open(a.baseline)) if a.baseline and os.path.exists(a.baseline) else None
    res = {
        'schema': 'imp2-sweep-result/1',
        'board': 'imp2',
        'run_number': int(a.run_number),
        'run_id': a.run_id,
        'family': a.family,
        'variant': a.variant,
        'status': a.status,
        'reason': a.reason,
        'reference': REF,
        'verdict_vs_reference': verdict(drc),
        'drc': drc,
        'baseline_drc': base,
        'board_md5': md5(a.board) if a.board and os.path.exists(a.board) else None,
        'finished_utc': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
    }
    with open(a.out, 'w') as fh:
        json.dump(res, fh, indent=2)
    print(json.dumps(res, indent=2))

if __name__ == '__main__':
    main()
