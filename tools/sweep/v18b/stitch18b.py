import sys, json, re, math, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew, blib18b as blib
from scipy import ndimage

# usage: stitch18b.py board drc.rpt out.json
# For each [unconnected] DRC pair (skipping bodge/frozen nets), emit a stitch track
# between the two items' @ points on a common layer, validated against the raster.
board, rpt, out = sys.argv[1], sys.argv[2], sys.argv[3]
SKIP = {'ROW3', 'CHG_PGOOD', 'ENC_PUSH'}
LAYI = {'F.Cu': 0, 'In1.Cu': 1, 'In2.Cu': 2, 'B.Cu': 3}
LAYN = {0: 'F_Cu', 1: 'In1_Cu', 2: 'In2_Cu', 3: 'B_Cu'}
b = pcbnew.LoadBoard(board)
base = blib.build_base(b)
codes = dict(base.netcode)
owner, holes = base.owner, base.holes
def dist_for(net):
    code = codes[net]
    outd = []
    for L in range(4):
        foreign = ((owner[L] != 0) & (owner[L] != code)) | (holes != 0)
        outd.append(ndimage.distance_transform_edt(~foreign) * blib.PITCH)
    return outd
def seg_ok(x1, y1, x2, y2, net, L, w, d, tol=0.03):
    n = max(2, int(math.hypot(x2 - x1, y2 - y1) / 0.04) + 1)
    for i in range(n + 1):
        x, y = x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n
        cx, cy = int(blib.cx(x)), int(blib.cy(y))
        if owner[L][cy][cx] not in (0, codes[net]) and d[L][cy][cx] < w / 2 + blib.CLR - tol:
            return False
    return True
txt = open(rpt).read()
blocks = re.split(r'\n(?=\[)', txt)
plan = {'codes': codes, 'add_tracks': [], 'add_vias': []}
done, failed = 0, []
for bl in blocks:
    if not bl.startswith('[unconnected'):
        continue
    items = re.findall(r"@\(([-\d.]+) mm, ([-\d.]+) mm\): ([^\n]+)", bl)
    if len(items) != 2:
        continue
    (x1, y1, d1), (x2, y2, d2) = items
    m = re.search(r'\[([A-Za-z0-9_]+)\]', d1)
    if not m:
        continue
    net = m.group(1)
    if net in SKIP:
        continue
    x1, y1, x2, y2 = float(x1), float(y1), float(x2), float(y2)
    def layer_of(desc):
        for k, v in LAYI.items():
            if 'on ' + k in desc:
                return v
        return 0  # pads default F
    L1, L2 = layer_of(d1), layer_of(d2)
    d = dist_for(net)
    w = 0.3 if net in ('GND', 'V3_3') else 0.25
    placed = False
    if L1 == L2:
        if seg_ok(x1, y1, x2, y2, net, L1, w, d):
            plan['add_tracks'].append({'net': net, 'layer': LAYN[L1], 'w': w, 'pts': [[x1, y1], [x2, y2]]})
            placed = True
        elif seg_ok(x1, y1, x2, y1, net, L1, w, d) and seg_ok(x2, y1, x2, y2, net, L1, w, d):
            plan['add_tracks'].append({'net': net, 'layer': LAYN[L1], 'w': w, 'pts': [[x1, y1], [x2, y1], [x2, y2]]})
            placed = True
    else:
        # via at midpoint-ish: try via at (x2,y1) corner on L1 then down, stub on L2
        vx, vy = x2, y1
        okv = all((owner[L][int(blib.cy(vy))][int(blib.cx(vx))] in (0, codes[net])) or d[L][int(blib.cy(vy))][int(blib.cx(vx))] >= blib.VIA_DIA / 2 + blib.CLR - 0.03 for L in range(4))
        if okv and seg_ok(x1, y1, vx, vy, net, L1, w, d) and seg_ok(vx, vy, x2, y2, net, L2, w, d):
            plan['add_vias'].append({'net': net, 'at': [vx, vy], 'dia': 0.6, 'drill': 0.3})
            plan['add_tracks'].append({'net': net, 'layer': LAYN[L1], 'w': w, 'pts': [[x1, y1], [vx, vy]]})
            plan['add_tracks'].append({'net': net, 'layer': LAYN[L2], 'w': w, 'pts': [[vx, vy], [x2, y2]]})
            placed = True
    if not placed and L1 == L2:
        # via-hop: stub on L1 from p1 to a via site, ride In2 (pour nets: owner==code
        # cells are exempt in seg_ok), via up near p2, stub to p2
        import math as _m
        def via_site(x, y):
            for r in (0.85, 1.05, 1.3, 1.6):
                for k in range(16):
                    a = 2 * _m.pi * k / 16
                    vx, vy = round(x + r * _m.cos(a), 2), round(y + r * _m.sin(a), 2)
                    cx, cy = int(blib.cx(vx)), int(blib.cy(vy))
                    if holes[cy][cx]:
                        continue
                    ok = True
                    for L in range(4):
                        if owner[L][cy][cx] not in (0, codes[net]) and d[L][cy][cx] < blib.VIA_DIA / 2 + blib.CLR - 0.03:
                            ok = False; break
                    if ok:
                        return (vx, vy)
            return None
        v1 = via_site(x1, y1)
        v2 = via_site(x2, y2)
        if v1 and v2 and seg_ok(x1, y1, v1[0], v1[1], net, L1, w, d) and seg_ok(v1[0], v1[1], v2[0], v2[1], net, 2, w, d) and seg_ok(v2[0], v2[1], x2, y2, net, L1, w, d):
            plan['add_vias'].append({'net': net, 'at': list(v1), 'dia': 0.6, 'drill': 0.3})
            plan['add_vias'].append({'net': net, 'at': list(v2), 'dia': 0.6, 'drill': 0.3})
            plan['add_tracks'].append({'net': net, 'layer': LAYN[L1], 'w': w, 'pts': [[x1, y1], list(v1)]})
            plan['add_tracks'].append({'net': net, 'layer': 'In2_Cu', 'w': w, 'pts': [list(v1), list(v2)]})
            plan['add_tracks'].append({'net': net, 'layer': LAYN[L1], 'w': w, 'pts': [list(v2), [x2, y2]]})
            placed = True
    if placed:
        done += 1
    else:
        failed.append((net, (round(x1, 2), round(y1, 2)), (round(x2, 2), round(y2, 2))))
json.dump(plan, open(out, 'w'))
print('stitches:', done, 'failed:', len(failed))
for f in failed:
    print('  FAIL', f)
