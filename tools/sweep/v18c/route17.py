import os
"""v17 batch closer (adapted from v9 batch_route.py): argv board, plan, log,
MF, then net:w1,w2 jobs. Hub-first comp pairing (small comp -> largest comp
first) so pour nets close fast. Plan checkpointed after every link."""
import sys, json, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blib
import numpy as np

BOARD, PLAN_OUT, LOGF = sys.argv[1], sys.argv[2], sys.argv[3]
blib.MF = float(sys.argv[4])
blib.CONGESTION_WEIGHT = float(os.environ.get("SWEEP_CONGESTION_WEIGHT", "0"))
LOG = open(LOGF, 'w')
JOBS = []
for spec in sys.argv[5:]:
    net, ws = spec.split(':')
    JOBS.append((net, [float(x) for x in ws.split(',')]))

def log(*a):
    s = ' '.join(str(x) for x in a)
    print(s); LOG.write(s + '\n'); LOG.flush()

b = blib.load(BOARD)
B = blib.build_base(b)
try:
    plan = json.load(open(PLAN_OUT))
except Exception:
    plan = {"codes": {}, "add_tracks": [], "add_vias": [], "delete": [], "delete_vias": []}
LNAME2 = {0: 'F_Cu', 1: 'In1_Cu', 2: 'In2_Cu', 3: 'B_Cu'}

def save():
    with open(PLAN_OUT, 'w') as f:
        json.dump(plan, f, indent=1)

def paint_route(net, tracks, vias, width):
    code = B.netcode[net]
    for (L, pts) in tracks:
        mask = np.zeros((blib.GH, blib.GW), dtype=np.uint8)
        im2, dr = blib._img(mask)
        for i in range(len(pts) - 1):
            blib._paint_seg(dr, pts[i][0], pts[i][1], pts[i+1][0], pts[i+1][1], width, 1)
        B.owner[L] = np.where(np.array(im2) > 0, code, B.owner[L])
    for (x, y) in vias:
        for L in range(4):
            mask = np.zeros((blib.GH, blib.GW), dtype=np.uint8)
            im2, dr = blib._img(mask)
            blib._paint_circle(dr, x, y, blib.VIA_DIA / 2, 1)
            B.owner[L] = np.where(np.array(im2) > 0, code, B.owner[L])
        mask = np.zeros((blib.GH, blib.GW), dtype=np.uint8)
        im2, dr = blib._img(mask)
        blib._paint_circle(dr, x, y, blib.VIA_DRILL / 2, 1)
        B.holes = np.where(np.array(im2) > 0, code, B.holes)
        B.anchors.setdefault(net, []).append((x, y))

def record(net, tracks, vias, width):
    plan['codes'][net] = B.netcode[net]
    for (L, pts) in tracks:
        plan['add_tracks'].append({"net": net, "layer": LNAME2[L], "pts": [[round(x, 3), round(y, 3)] for (x, y) in pts], "w": width})
    for (x, y) in vias:
        plan['add_vias'].append({"net": net, "at": [round(x, 3), round(y, 3)], "dia": blib.VIA_DIA, "drill": blib.VIA_DRILL})
    save()

def close_net(net, widths, max_iter=60):
    landed = 0
    for it in range(max_iter):
        lab, members, padlab = blib.net_components(B, net)
        if len(members) <= 1:
            log("NET %s: unified after %d links" % (net, landed))
            return landed, True
        order = sorted(members.keys(), key=lambda c: (len(members[c]), c))
        big = max(members.keys(), key=lambda c: len(members[c]))
        progressed = False
        for a in order:
            if a == big and len(members) > 2:
                continue
            targets = [big] + [c for c in order if c != big and c != a]
            for bb in targets:
                if bb == a: continue
                for w in widths:
                    t0 = time.time()
                    res = blib.route(B, net, lab, a, bb, width=w)
                    dt = time.time() - t0
                    if res is not None:
                        tracks, vias = res
                        paint_route(net, tracks, vias, w)
                        record(net, tracks, vias, w)
                        landed += 1; progressed = True
                        log("NET %s: link %s->%s w=%.2f %.1fs tracks=%d vias=%d (pads %s)" % (
                            net, a, bb, w, dt, len(tracks), len(vias), sorted(members[a])))
                        break
                if progressed: break
            if progressed: break
        if not progressed:
            log("NET %s: SEALED with %d comps: %s" % (net, len(members), {c: sorted(m) for c, m in members.items()}))
            return landed, False
    log("NET %s: iteration cap" % net)
    return landed, False

total = 0
for (net, widths) in JOBS:
    n, ok = close_net(net, widths)
    total += n
    log("== %s: %d links landed, unified=%s" % (net, n, ok))
save()
log("TOTAL links:", total, "plan tracks:", len(plan['add_tracks']), "vias:", len(plan['add_vias']))
LOG.close()
