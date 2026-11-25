"""Fail-fast gate for one IMP-2 routing stage.

Usage: python3 check_failfast_imp2.py <prev_counts.json> <cur_counts.json>
Exit 0 prints CONTINUE, exit 2 prints STOP with the reason.

Reference is the IMP-2 layout pass 2 board: 134 unconnected / 0 shorts /
12 clearance (the 12 fenced at U12's 0.4 mm-pitch pads). STOP rules:
  - shorts appeared (> 0)
  - clearance grew vs the previous stage (the fenced 12 may hold, never grow)
  - unconnected is still >= 134 AND the stage closed nothing
    (cur.unconnected >= prev.unconnected while >= 134)
A stage that closed at least one net, or is already below 134, continues.
"""
import json, sys

REF_UNCONNECTED = 134

def main():
    prev = json.load(open(sys.argv[1]))
    cur = json.load(open(sys.argv[2]))
    if cur.get('shorts', 0) > 0:
        print('STOP: shorts appeared (%d)' % cur['shorts'])
        return 2
    if cur.get('clearance', 0) > prev.get('clearance', 0):
        print('STOP: clearance grew %d -> %d' % (prev.get('clearance', 0), cur.get('clearance', 0)))
        return 2
    if cur.get('unconnected', 999) >= REF_UNCONNECTED and cur.get('unconnected', 999) >= prev.get('unconnected', 999):
        print('STOP: no improvement at %d unconnected (reference is 134)' % cur.get('unconnected', 999))
        return 2
    print('CONTINUE: unconnected %s -> %s' % (prev.get('unconnected'), cur.get('unconnected')))
    return 0

if __name__ == '__main__':
    sys.exit(main())
