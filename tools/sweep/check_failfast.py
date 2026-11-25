"""Fail-fast gate for one routing stage.

Usage: python3 check_failfast.py <prev_counts.json> <cur_counts.json>
Exit 0 prints CONTINUE, exit 2 prints STOP with the reason.

STOP rules (a stage that cannot beat v15's 15/0/0 is not worth more stages):
  - shorts appeared (> 0)
  - clearance grew vs the previous stage
  - unconnected is still >= 15 AND the stage closed nothing
    (cur.unconnected >= prev.unconnected while >= 15)
A stage that closed at least one net, or is already below 15, continues.
"""
import json, sys

def main():
    prev = json.load(open(sys.argv[1]))
    cur = json.load(open(sys.argv[2]))
    if cur.get('shorts', 0) > 0:
        print('STOP: shorts appeared (%d)' % cur['shorts'])
        return 2
    if cur.get('clearance', 0) > prev.get('clearance', 0):
        print('STOP: clearance grew %d -> %d' % (prev.get('clearance', 0), cur.get('clearance', 0)))
        return 2
    if cur.get('unconnected', 999) >= 15 and cur.get('unconnected', 999) >= prev.get('unconnected', 999):
        print('STOP: no improvement at %d unconnected (v15 is 15)' % cur.get('unconnected', 999))
        return 2
    print('CONTINUE: unconnected %s -> %s' % (prev.get('unconnected'), cur.get('unconnected')))
    return 0

if __name__ == '__main__':
    sys.exit(main())
