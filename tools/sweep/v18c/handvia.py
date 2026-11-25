import os
import sys, json, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew, blib

# usage: handvia.py board out.json NET:REF.PIN [NET:REF.PIN ...]
# for each pad: find a via site (ring search) whose disc clears foreign copper on all
# layers (raster d >= VIA_R+CLR for foreign; own-net cells exempt) and that sits on (or
# reaches) the net's In2 pour; connect pad->via with an L path on F (or B for B pads).
board, out = sys.argv[1], sys.argv[2]
targets = sys.argv[3:]
blib.MF = 0.0
b = pcbnew.LoadBoard(board)
base = blib.build_base(b)
codes = {}
for n in b.GetNetsByName():
    codes[str(n)] = b.GetNetsByName()[n].GetNetCode()
padpos = {}
for fp in b.GetFootprints():
    for p in fp.Pads():
        padpos[(fp.GetReference(), p.GetNumber())] = (blib.mm(p.GetPosition().x), blib.mm(p.GetPosition().y), p.IsOnLayer(pcbnew.B_Cu) and not p.IsOnLayer(pcbnew.F_Cu))
from scipy import ndimage
plan = {'codes': codes, 'add_tracks': [], 'add_vias': []}
owner = base.owner
zfill = base.zfill
holes = base.holes
def dist_for(net):
    code = codes[net]
    out = []
    for L in range(4):
        foreign = ((owner[L] != 0) & (owner[L] != code)) | (holes != 0)
        out.append(ndimage.distance_transform_edt(~foreign) * blib.PITCH)
    return out
def cell(x, y): return (int(blib.cx(x)), int(blib.cy(y)))
def foreign_clear(x, y, net, need, d):
    c = cell(x, y)
    for L in range(4):
        o = owner[L][c[1]][c[0]]
        if o not in (0, codes[net]) and d[L][c[1]][c[0]] < need:
            return False
        if holes[c[1]][c[0]] != 0:
            return False
    return True
def on_pour(x, y, net):
    # DEEP pour: full 0.75-radius disc on In2 must be this net's fill/owner,
    # so KiCad's own refill (which carves at foreign-copper edges) still touches the via
    code = codes[net]
    for dx in range(-15, 16, 3):
        for dy in range(-15, 16, 3):
            if dx * dx + dy * dy > 225:
                continue
            cx, cy = int(blib.cx(x)) + dx // 3, int(blib.cy(y)) + dy // 3
            # sample at 0.15 steps over the disc via direct offsets
            sx, sy = x + dx * 0.05, y + dy * 0.05
            ccx, ccy = int(blib.cx(sx)), int(blib.cy(sy))
            if zfill[2][ccy][ccx] != code and owner[2][ccy][ccx] != code:
                return False
    return True
def path_clear(x1, y1, x2, y2, net, L, w, d):
    # L path: (x1,y1)->(x2,y1)->(x2,y2), sample every 0.05
    pts = []
    n1 = max(2, int(abs(x2 - x1) / 0.05) + 1)
    for i in range(n1 + 1): pts.append((x1 + (x2 - x1) * i / n1, y1))
    n2 = max(2, int(abs(y2 - y1) / 0.05) + 1)
    for i in range(1, n2 + 1): pts.append((x2, y1 + (y2 - y1) * i / n2))
    for (x, y) in pts:
        c = cell(x, y)
        o = owner[L][c[1]][c[0]]
        if o not in (0, codes[net]) and d[L][c[1]][c[0]] < w / 2 + blib.CLR:
            return False
    return True
for tg in targets:
    net, rp = tg.split(':')
    ref, pin = rp.split('.')
    x, y, is_b = padpos[(ref, pin)]
    d = dist_for(net)
    L = 3 if is_b else 0
    Lname = 'B_Cu' if is_b else 'F_Cu'
    found = None
    for r in [0.85, 1.0, 1.15, 1.3, 1.5, 1.7, 1.95, 2.2, 2.5]:
        for k in range(24):
            a = 2 * math.pi * k / 24
            vx, vy = round(x + r * math.cos(a), 2), round(y + r * math.sin(a), 2)
            if not foreign_clear(vx, vy, net, blib.VIA_DIA / 2 + blib.CLR, d):
                continue
            if not on_pour(vx, vy, net):
                continue
            if path_clear(x, y, vx, vy, net, L, 0.3, d):
                found = (vx, vy); break
        if found: break
    if not found:
        print('NO SITE', tg); continue
    vx, vy = found
    plan['add_vias'].append({'net': net, 'at': [vx, vy], 'dia': 0.6, 'drill': 0.3})
    plan['add_tracks'].append({'net': net, 'layer': Lname, 'w': 0.3, 'pts': [[x, y], [vx, y], [vx, vy]]})
    print('SEEDED', tg, 'via at', found)
json.dump(plan, open(out, 'w'))
print('plan vias:', len(plan['add_vias']))
