#!/usr/bin/env python3
"""v18a sweep: inject a Freerouting (autoroute_settings ...) block into a
KiCad-exported DSN, as a direct child of (pcb ...), per Freerouting 1.9.0
source (designforms/specctra/AutorouteSettings.java: scope keyword
'autoroute_settings', single-integer (via_costs N) / (plane_via_costs N) /
(start_ripup_costs N) / (start_pass_no N); defaults via_costs=50,
plane_via_costs=5, start_ripup_costs=100 — interactive/AutorouteSettings.java).
PORT NOTE (tools/sweep-v18a): unchanged from the local
v18a-sweep_work/inject_autoroute.py except this header note. No paths in
this file at all.
Usage: inject_autoroute.py <in.dsn> <out.dsn> [--via-costs N]
  [--plane-via-costs N] [--start-ripup-costs N] [--start-pass-no N]
Only the flags given are overridden; the rest stay at Freerouting defaults
(the block lists the overridden values plus on/off lines)."""
import sys, argparse
p = argparse.ArgumentParser()
p.add_argument('inp'); p.add_argument('outp')
p.add_argument('--via-costs', type=int, default=None)
p.add_argument('--plane-via-costs', type=int, default=None)
p.add_argument('--start-ripup-costs', type=int, default=None)
p.add_argument('--start-pass-no', type=int, default=None)
a = p.parse_args()
txt = open(a.inp).read()
block = ['  (autoroute_settings',
         '    (fanout off)',
         '    (autoroute on)',
         '    (postroute on)',
         '    (vias on)']
if a.via_costs is not None:
    block.append('    (via_costs %d)' % a.via_costs)
if a.plane_via_costs is not None:
    block.append('    (plane_via_costs %d)' % a.plane_via_costs)
if a.start_ripup_costs is not None:
    block.append('    (start_ripup_costs %d)' % a.start_ripup_costs)
if a.start_pass_no is not None:
    block.append('    (start_pass_no %d)' % a.start_pass_no)
block.append('  )')
ins = '\n'.join(block) + '\n'
marker = '  (structure\n'
assert marker in txt, 'structure marker not found'
txt = txt.replace(marker, ins + marker, 1)
open(a.outp, 'w').write(txt)
print('injected:', [l.strip() for l in block[1:-1]])
