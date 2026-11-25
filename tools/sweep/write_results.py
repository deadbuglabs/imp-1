"""Assemble one sweep job's results JSON.

Usage:
  python3 write_results.py --family v18a --variant astar-0.05 \
    --run-number 12 --run-id 123456789 --counts out/counts.json \
    --board out/board.kicad_pcb --status done|blocked|failfast|toolchain-ok \
    --reason '...' --out out/v18a-astar-0.05.json [--baseline out/base_counts.json]

Verdict vs the v15 reference (15 unconnected / 0 shorts / 0 clearance):
  beat    unconnected < 15 and shorts == 0 and clearance == 0
  tie     unconnected == 15 and shorts == 0 and clearance == 0
  no-beat anything else (or no DRC at all -> n/a)
"""
import argparse, datetime, hashlib, json, os, sys

V15 = {'unconnected': 15, 'shorts': 0, 'clearance': 0}

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
    if u < V15['unconnected'] and drc.get('shorts', 0) == 0 and drc.get('clearance', 0) == 0:
        return 'beat'
    if u == V15['unconnected'] and drc.get('shorts', 0) == 0 and drc.get('clearance', 0) == 0:
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
        'schema': 'imp1-sweep-result/1',
        'run_number': int(a.run_number),
        'run_id': a.run_id,
        'family': a.family,
        'variant': a.variant,
        'status': a.status,
        'reason': a.reason,
        'v15_reference': V15,
        'verdict_vs_v15': verdict(drc),
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
