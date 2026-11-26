import os
"""Edit-only applier: fresh load -> delete targets -> save -> fresh load -> adds -> save.
No board enumeration after edits within a process phase. Usage: apply_plan.py <board> <plan.json>"""
import sys, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blib18cr3 as blib, pcbnew

board_path, plan_path = sys.argv[1], sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else 'all'
plan = json.load(open(plan_path))
LAY = {'F_Cu': pcbnew.F_Cu, 'In1_Cu': pcbnew.In1_Cu, 'In2_Cu': pcbnew.In2_Cu, 'B_Cu': pcbnew.B_Cu}

# phase 1: deletions (run as its own process: one board load per process)
if (plan.get('delete') or plan.get('delete_vias')) and mode in ('all', 'delete'):
    b = blib.load(board_path)
    n = 0
    nv = 0
    for t in list(b.GetTracks()):
        if isinstance(t, pcbnew.PCB_VIA):
            for d in plan.get('delete_vias', []):
                if t.GetNetname() != d['net']: continue
                p = t.GetPosition()
                if abs(blib.mm(p.x) - d['at'][0]) < 0.01 and abs(blib.mm(p.y) - d['at'][1]) < 0.01:
                    b.Remove(t); nv += 1; print("deleted via", d['net'], d['at'])
            continue
        for d in plan.get('delete', []):
            if t.GetNetname() != d['net'] or t.GetLayer() != LAY[d['layer']]: continue
            s, e = t.GetStart(), t.GetEnd()
            got = (blib.mm(s.x), blib.mm(s.y), blib.mm(e.x), blib.mm(e.y))
            want = tuple(d['pts']); want_r = (want[2], want[3], want[0], want[1])
            def close(a, bb): return all(abs(x - y) < 0.011 for x, y in zip(a, bb))
            if close(got, want) or close(got, want_r):
                b.Remove(t); n += 1; print("deleted", d['net'], tuple(round(v, 4) for v in got))
    print("deletions:", n, "of", len(plan.get('delete', [])), "; vias:", nv, "of", len(plan.get('delete_vias', [])))
    if n != len(plan.get('delete', [])) or nv != len(plan.get('delete_vias', [])):
        print("WARNING: deletion count mismatch -- saving anyway; DRC arbitrates")
    blib.save(b, board_path)
    sys.exit(0)

# phase 2: additions (own process, fresh load, no enumeration at all)
if (plan.get('add_tracks') or plan.get('add_vias')) and mode in ('all', 'add'):
    b = blib.load(board_path)
    for a in plan.get('add_vias', []):
        v = pcbnew.PCB_VIA(b)
        v.SetPosition(pcbnew.VECTOR2I(blib.nm(a['at'][0]), blib.nm(a['at'][1])))
        v.SetWidth(blib.nm(a['dia'])); v.SetDrill(blib.nm(a['drill']))
        v.SetViaType(3)
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        v.SetNetCode(plan['codes'][a['net']])
        b.Add(v); print("added via", a['net'], a['at'], a['dia'], a['drill'])
    for a in plan.get('add_tracks', []):
        pts = a['pts']
        for i in range(len(pts) - 1):
            t = pcbnew.PCB_TRACK(b)
            t.SetStart(pcbnew.VECTOR2I(blib.nm(pts[i][0]), blib.nm(pts[i][1])))
            t.SetEnd(pcbnew.VECTOR2I(blib.nm(pts[i+1][0]), blib.nm(pts[i+1][1])))
            t.SetWidth(blib.nm(a['w'])); t.SetLayer(LAY[a['layer']])
            t.SetNetCode(plan['codes'][a['net']])
            b.Add(t); print("added track", a['net'], a['layer'], pts[i], "->", pts[i+1], "w", a['w'])
    blib.save(b, board_path)
print("plan applied")
