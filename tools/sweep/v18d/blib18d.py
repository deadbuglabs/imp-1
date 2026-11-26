"""IMP-1 main v8 routing library: rasterize board, label net components, A* route."""
import pcbnew, wx, math, heapq
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

PITCH = 0.05            # mm per cell
OX, OY = -1.0, -1.0     # grid origin (mm)
GW, GH = 2200, 1240     # cells: x -1..109, y -1..61 (v18d: east edge 108)
LAYERS = [pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu]
LNAME = {pcbnew.F_Cu: 'F', pcbnew.In1_Cu: 'In1', pcbnew.In2_Cu: 'In2', pcbnew.B_Cu: 'B'}
CLR = 0.20              # default netclass clearance mm
VIA_DIA, VIA_DRILL = 0.60, 0.30
BOARD_POLY = [(0,5),(36,0),(36,-1),(100,-1),(108,5),(108,55),(100,60.8),(36,60.8),(0,55)]  # v18d
BAY = (36.0, 19.43, 103.0, 57.43)  # v18d: bay +1.43 south (capped by J12/IR east clr)

_app = None
def app():
    global _app
    if _app is None:
        _app = wx.App()
    return _app

def load(path):
    app()
    return pcbnew.LoadBoard(path)

def refill(board):
    f = pcbnew.ZONE_FILLER(board)
    f.Fill(board.Zones())

def save(board, path):
    refill(board)
    pcbnew.SaveBoard(path, board)

def mm(v): return v / 1e6
def nm(v): return int(round(v * 1e6))
def cx(x): return (x - OX) / PITCH
def cy(y): return (y - OY) / PITCH
def cellxy(cxx, cyy): return (OX + cxx * PITCH, OY + cyy * PITCH)

# ---------------- raster painting helpers (work in cell coords) ----------------
def _img(arr):
    im = Image.fromarray(arr)
    return im, ImageDraw.Draw(im)

def _poly_pts(pts_mm):
    return [(cx(x), cy(y)) for (x, y) in pts_mm]

def _paint_pad(dr, pad, value):
    pos = pad.GetPosition()
    px, py = mm(pos.x), mm(pos.y)
    sx, sy = mm(pad.GetSize().x), mm(pad.GetSize().y)
    shape = pad.GetShape()
    ang = math.radians(pad.GetOrientation().AsDegrees())
    ca, sa = math.cos(ang), math.sin(ang)
    if shape == 0:  # circle
        r = sx / 2
        dr.ellipse([cx(px-r), cy(py-r), cx(px+r), cy(py+r)], fill=value)
        return
    # local corners
    hx, hy = sx/2, sy/2
    corners = []
    for (lx, ly) in [(-hx,-hy),(hx,-hy),(hx,hy),(-hx,hy)]:
        corners.append((px + lx*ca - ly*sa, py + lx*sa + ly*ca))
    if shape == 2:  # oval: stadium outline (two semicircles + straights), rotated frame
        pts = []
        if sx >= sy:
            r = hy; off = hx - r
            for i in range(19):
                t = -math.pi/2 + i*math.pi/18
                lx, ly = off + r*math.cos(t), r*math.sin(t)
                pts.append((px + lx*ca - ly*sa, py + lx*sa + ly*ca))
            for i in range(19):
                t = math.pi/2 + i*math.pi/18
                lx, ly = -off + r*math.cos(t), r*math.sin(t)
                pts.append((px + lx*ca - ly*sa, py + lx*sa + ly*ca))
        else:
            r = hx; off = hy - r
            for i in range(19):
                t = i*math.pi/18
                lx, ly = r*math.cos(t), off + r*math.sin(t)
                pts.append((px + lx*ca - ly*sa, py + lx*sa + ly*ca))
            for i in range(19):
                t = math.pi + i*math.pi/18
                lx, ly = r*math.cos(t), -off + r*math.sin(t)
                pts.append((px + lx*ca - ly*sa, py + lx*sa + ly*ca))
        dr.polygon(_poly_pts(pts), fill=value)
        return
    if shape == 4:  # roundrect: rounded-corner outline, radius capped at 0.30
        # (cap guards against the API overstating r on big pads like SW1 MP,
        #  which would shrink the painted corner vs true copper)
        try:
            r = min(mm(pad.GetRoundRectCornerRadius()), hx, hy, 0.30)
        except Exception:
            r = 0.25 * min(sx, sy)
        if r <= 0.01:
            dr.polygon(_poly_pts(corners), fill=value)
            return
        pts = []
        for (ccx, ccy, a0) in [(hx-r, -hy+r, -90.0), (hx-r, hy-r, 0.0), (-hx+r, hy-r, 90.0), (-hx+r, -hy+r, 180.0)]:
            for i in range(7):
                t = math.radians(a0 + i*90.0/6)
                lx, ly = ccx + r*math.cos(t), ccy + r*math.sin(t)
                pts.append((px + lx*ca - ly*sa, py + lx*sa + ly*ca))
        dr.polygon(_poly_pts(pts), fill=value)
        return
    # rect / trapezoid / chamfered / custom -> bbox polygon (conservative)
    dr.polygon(_poly_pts(corners), fill=value)

def _paint_seg(dr, x0, y0, x1, y1, w_mm, value):
    r = w_mm / 2
    dr.line([cx(x0), cy(y0), cx(x1), cy(y1)], fill=value, width=max(1, int(round(w_mm/PITCH))))
    for (x, y) in [(x0, y0), (x1, y1)]:
        dr.ellipse([cx(x-r), cy(y-r), cx(x+r), cy(y+r)], fill=value)

def _paint_circle(dr, x, y, r_mm, value):
    dr.ellipse([cx(x-r_mm), cy(y-r_mm), cx(x+r_mm), cy(y+r_mm)], fill=value)

def _paint_polyset(dr, ps, value, clear_value=None):
    for i in range(ps.OutlineCount()):
        ch = ps.Outline(i)
        pts = [(mm(ch.CPoint(j).x), mm(ch.CPoint(j).y)) for j in range(ch.PointCount())]
        if len(pts) >= 3:
            dr.polygon(_poly_pts(pts), fill=value)
        for h in range(ps.HoleCount(i)):
            hc = ps.Hole(i, h)
            hpts = [(mm(hc.CPoint(j).x), mm(hc.CPoint(j).y)) for j in range(hc.PointCount())]
            if len(hpts) >= 3 and clear_value is not None:
                dr.polygon(_poly_pts(hpts), fill=clear_value)

class Base:
    pass

def build_base(board):
    """Paint hard copper (owner), drill holes, zone fills, hard-blocked mask."""
    B = Base()
    # net name -> code map from board netinfo
    B.netcode = {}
    ni = board.GetNetInfo()
    for code in range(ni.GetNetCount()):
        item = ni.GetNetItem(code)
        if item is not None:
            B.netcode[item.GetNetname()] = code
    B.owner = [np.zeros((GH, GW), dtype=np.int32) for _ in range(4)]
    B.holes = np.zeros((GH, GW), dtype=np.int32)
    B.zfill = [np.zeros((GH, GW), dtype=np.int32) for _ in range(4)]
    B.hard = [np.zeros((GH, GW), dtype=bool) for _ in range(4)]  # keepout/outside: no tracks/vias
    B.hard_keep = [np.zeros((GH, GW), dtype=bool) for _ in range(4)]  # keepout zones only
    B.hard_edge = np.zeros((GH, GW), dtype=bool)  # outside-board + bay (all layers)
    ims = []
    drs = []
    for L in range(4):
        im, dr = _img(B.owner[L]); ims.append(im); drs.append(dr)
    him, hdr = _img(B.holes)
    # board outline + bay -> hard mask painted later as bool
    # pads
    B.pads = {}   # (ref, pin) -> dict
    B.net_pads = {}
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        for pad in fp.Pads():
            net = pad.GetNetname()
            if not net:
                # no-net pad: foreign copper for every net; its drill blocks vias
                for L in range(4):
                    if pad.IsOnLayer(LAYERS[L]):
                        _paint_pad(drs[L], pad, -1)
                d0 = mm(pad.GetDrillSize().x)
                if d0 > 0:
                    pp = pad.GetPosition()
                    _paint_circle(hdr, mm(pp.x), mm(pp.y), d0/2, -1)
                continue
            code = pad.GetNetCode()
            pos = pad.GetPosition()
            info = dict(ref=ref, pin=pad.GetNumber(), x=mm(pos.x), y=mm(pos.y),
                        sx=mm(pad.GetSize().x), sy=mm(pad.GetSize().y),
                        shape=pad.GetShape(), drill=mm(pad.GetDrillSize().x),
                        layers=[L for L in range(4) if pad.IsOnLayer(LAYERS[L])],
                        net=net)
            if info['drill'] > 0:
                info['layers'] = [0, 1, 2, 3]
            B.pads[(ref, pad.GetNumber())] = info
            B.net_pads.setdefault(net, []).append(info)
            for L in info['layers']:
                _paint_pad(drs[L], pad, code)
            if info['drill'] > 0:
                _paint_circle(hdr, info['x'], info['y'], info['drill']/2, code)
    # tracks + vias
    B.anchors = {}  # net -> list of (x,y) vertical connection columns (vias + PTH pads)
    for (ref, pin), info in B.pads.items():
        if info['drill'] > 0:
            B.anchors.setdefault(info['net'], []).append((info['x'], info['y']))
    for t in board.GetTracks():
        net = t.GetNetname()
        if not net:
            continue
        code = t.GetNetCode()
        if isinstance(t, pcbnew.PCB_VIA):
            pos = t.GetPosition()
            x, y = mm(pos.x), mm(pos.y)
            for L in range(4):
                _paint_circle(drs[L], x, y, mm(t.GetWidth())/2, code)
            _paint_circle(hdr, x, y, mm(t.GetDrill())/2, code)
            B.anchors.setdefault(net, []).append((x, y))
        else:
            L = LAYERS.index(t.GetLayer())
            s, e = t.GetStart(), t.GetEnd()
            _paint_seg(drs[L], mm(s.x), mm(s.y), mm(e.x), mm(e.y), mm(t.GetWidth()), code)
    for L in range(4):
        B.owner[L] = np.array(ims[L])
    B.holes = np.array(him)
    # zone fills + keepouts
    for z in board.Zones():
        net = z.GetNetname()
        ls = z.GetLayerSet()
        zlayers = [L for L in range(4) if ls.Contains(LAYERS[L])]
        if net:  # copper pour
            code = z.GetNetCode()
            for L in zlayers:
                im, dr = _img(B.zfill[L].copy())
                _paint_polyset(dr, z.GetFilledPolysList(LAYERS[L]), code, clear_value=0)
                B.zfill[L] = np.array(im)
        else:    # keepout
            for L in zlayers:
                im, dr = _img(B.hard[L].astype(np.uint8))
                _paint_polyset(dr, z.Outline(), 1)
                B.hard[L] = np.array(im).astype(bool)
                B.hard_keep[L] = B.hard_keep[L] | B.hard[L]
    # footprint-embedded rule areas (e.g. SW1 'Shell contact', U1 antenna area):
    # outlines are already in board coordinates
    for fp in board.GetFootprints():
        for z in fp.Zones():
            if z.GetNetname():
                continue
            ls = z.GetLayerSet()
            for L in range(4):
                if not ls.Contains(LAYERS[L]):
                    continue
                im, dr = _img(B.hard[L].astype(np.uint8))
                _paint_polyset(dr, z.Outline(), 1)
                B.hard[L] = np.array(im).astype(bool)
                B.hard_keep[L] = B.hard_keep[L] | B.hard[L]
    # outside board + bay -> hard on all layers
    edge = np.zeros((GH, GW), dtype=bool)
    for L in range(4):
        m = np.ones((GH, GW), dtype=np.uint8)
        im = Image.fromarray(m); dr = ImageDraw.Draw(im)
        dr.polygon(_poly_pts(BOARD_POLY), fill=0)
        B.hard[L] |= (np.array(im) > 0)
        edge |= (np.array(im) > 0)
        im2 = Image.fromarray(np.zeros((GH, GW), dtype=np.uint8)); dr2 = ImageDraw.Draw(im2)
        dr2.rectangle([cx(BAY[0]), cy(BAY[1]), cx(BAY[2]), cy(BAY[3])], fill=1)
        B.hard[L] |= (np.array(im2) > 0)
        edge |= (np.array(im2) > 0)
    B.hard_edge = edge
    return B

# ---------------- components ----------------
def net_components(B, net):
    """Return (labelgrid[4], {label: set((ref,pin))}, pad->label)."""
    code = B.netcode[net]
    own = []
    for L in range(4):
        o = (B.owner[L] == code)
        if net in ('GND', 'V3_3'):
            o = o | (B.zfill[L] == code)
        own.append(o)
    struct = np.array([[[0,0,0],[0,1,0],[0,0,0]],[[1,1,1],[1,1,1],[1,1,1]],[[0,0,0],[0,1,0],[0,0,0]]])
    lab = []
    nxt = 0
    for L in range(4):
        l, n = ndimage.label(own[L], structure=np.array([[1,1,1],[1,1,1],[1,1,1]]))
        l = np.where(l > 0, l + nxt, 0)
        nxt += n
        lab.append(l.astype(np.int32))
    parent = list(range(nxt + 1))
    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]; a = parent[a]
        return a
    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb: parent[ra] = rb
    for (x, y) in B.anchors.get(net, []):
        ccx, cyy = int(round(cx(x))), int(round(cy(y)))
        ids = [lab[L][cyy, ccx] for L in range(4) if lab[L][cyy, ccx] > 0]
        for i in ids[1:]:
            union(ids[0], i)
    out = [np.zeros((GH, GW), dtype=np.int32) for _ in range(4)]
    members = {}
    for L in range(4):
        m = lab[L] > 0
        roots = np.zeros_like(lab[L])
        for old in np.unique(lab[L][m]):
            roots[lab[L] == old] = find(old)
        out[L] = roots
    padlab = {}
    for info in B.net_pads.get(net, []):
        ccx, cyy = int(round(cx(info['x']))), int(round(cy(info['y'])))
        got = 0
        for L in info['layers']:
            if out[L][cyy, ccx] > 0:
                got = out[L][cyy, ccx]; break
        if got:
            padlab[(info['ref'], info['pin'])] = got
            members.setdefault(got, set()).add((info['ref'], info['pin']))
    return out, members, padlab

def label_at(B, lab, net, x, y, layer=None):
    ccx, cyy = int(round(cx(x))), int(round(cy(y)))
    rng = [layer] if layer is not None else range(4)
    for L in rng:
        if 0 <= cyy < GH and 0 <= ccx < GW and lab[L][cyy, ccx] > 0:
            return lab[L][cyy, ccx]
    # search small neighborhood
    for r in range(1, 8):
        for L in rng:
            sub = lab[L][max(0, cyy-r):cyy+r+1, max(0, ccx-r):ccx+r+1]
            nz = np.nonzero(sub)
            if len(nz[0]):
                return sub[nz[0][0], nz[1][0]]
    return 0

# ---------------- A* router ----------------
MF = 0.09   # foreign-copper margin (scout runs may relax; DRC arbitrates)
MK = 0.05   # keepout/edge margin

def route(B, net, lab, compA, compB, width=0.25, via_cost=30.0, extra_block=None, maxpop=400000):
    """A* from any cell of compA to any cell of compB. Returns (tracks, vias):
    tracks = list of (layer_idx, [(x,y),...]) mm points; vias = [(x,y)] mm.
    maxpop caps node expansions: a sealed search otherwise runs until the
    heap empties (millions of nodes, 10+ min). Landed links take 3-6s
    (tens of thousands of pops), so 400k does not cut real routes."""
    code = B.netcode[net]
    # SAFETY: raster painting shrinks true copper by up to ~1 cell at edges and
    # distance is measured to cell centers -- demand real clearance + margin.
    M_F = MF
    M_K = MK
    r_inf = CLR + width/2.0 + M_F
    EDGE_CLR = 0.20  # min_copper_edge_clearance from .kicad_pro
    dedge = ndimage.distance_transform_edt(~B.hard_edge) * PITCH
    dist = []
    passable = []
    for L in range(4):
        foreign = (B.owner[L] != 0) & (B.owner[L] != code)
        d = ndimage.distance_transform_edt(~foreign) * PITCH
        dkeep = ndimage.distance_transform_edt(~B.hard_keep[L]) * PITCH
        p = (d >= r_inf - 1e-9) & (dkeep >= width/2 + M_K - 1e-9) & (dedge >= EDGE_CLR + width/2 + M_K - 1e-9)
        p |= (lab[L] == compA) | (lab[L] == compB)
        if extra_block is not None:
            p &= ~extra_block[L]
        dist.append(d); passable.append(p)
    # via feasibility: all-layer copper distance with via radius + hole distance
    dvia = None
    for L in range(4):
        foreign = (B.owner[L] != 0) & (B.owner[L] != code)
        d = ndimage.distance_transform_edt(~foreign) * PITCH
        dvia = d if dvia is None else np.minimum(dvia, d)
    hdist = ndimage.distance_transform_edt(B.holes == 0) * PITCH
    via_ok = (dvia >= CLR + VIA_DIA/2 + M_F - 1e-9) & (hdist >= 0.50 - 1e-9)
    via_ok &= (dedge >= EDGE_CLR + VIA_DIA/2 + M_K - 1e-9)
    for L in range(4):
        dk = ndimage.distance_transform_edt(~B.hard_keep[L]) * PITCH
        via_ok &= (dk >= VIA_DIA/2 + M_K - 1e-9)
    # A*
    INF = float('inf')
    g = [np.full((GH, GW), INF, dtype=np.float64) for _ in range(4)]
    parent = [np.full((GH, GW), -1, dtype=np.int32) for _ in range(4)]
    heap = []
    # seed: boundary cells of compA (cells in A adjacent to non-A passable) + all A cells adjacent to B handled by goal test on pop
    seedmask = np.zeros((GH, GW), dtype=bool)
    for L in range(4):
        a = (lab[L] == compA)
        if not a.any(): continue
        dil = ndimage.binary_dilation(a, structure=np.array([[1,1,1],[1,1,1],[1,1,1]]))
        frontier = a & dil & ~(a & passable[L] & (ndimage.binary_erosion(a, structure=np.array([[1,1,1],[1,1,1],[1,1,1]]))))
        frontier = a & (ndimage.binary_dilation(~a, structure=np.array([[1,1,1],[1,1,1],[1,1,1]])))  # A cells touching non-A
        frontier |= a & (lab[L] == compB)
        ys, xs = np.nonzero(frontier)
        for yy, xx in zip(ys, xs):
            if g[L][yy, xx] > 0:
                g[L][yy, xx] = 0
                heapq.heappush(heap, (0.0, 0.0, L, int(yy), int(xx)))
    # heuristic: euclidean distance (mm) to nearest compB cell, min over layers (admissible)
    hgrid = None
    for L in range(4):
        d = ndimage.distance_transform_edt(lab[L] != compB) * PITCH
        hgrid = d if hgrid is None else np.minimum(hgrid, d)
    goal = None
    npop = 0
    DIRS = [(-1,-1,1.41421),(0,-1,1.0),(1,-1,1.41421),(-1,0,1.0),(1,0,1.0),(-1,1,1.41421),(0,1,1.0),(1,1,1.41421)]
    while heap:
        _f, cost, L, yy, xx = heapq.heappop(heap)
        npop += 1
        if npop > maxpop:
            return None
        if cost > g[L][yy, xx] + 1e-6: continue
        if lab[L][yy, xx] == compB:
            goal = (L, yy, xx); break
        for dx, dy, w in DIRS:
            nx, ny = xx+dx, yy+dy
            if not (0 <= nx < GW and 0 <= ny < GH): continue
            if not passable[L][ny, nx]: continue
            nc = cost + w * PITCH
            if nc < g[L][ny, nx] - 1e-9:
                g[L][ny, nx] = nc
                parent[L][ny, nx] = (L * GH + yy) * GW + xx
                heapq.heappush(heap, (nc + hgrid[ny, nx], nc, L, ny, nx))
        if via_ok[yy, xx]:
            for L2 in range(4):
                if L2 == L: continue
                if not passable[L2][yy, xx]: continue
                nc = cost + via_cost * PITCH
                if nc < g[L2][yy, xx] - 1e-9:
                    g[L2][yy, xx] = nc
                    parent[L2][yy, xx] = (L * GH + yy) * GW + xx
                    heapq.heappush(heap, (nc + hgrid[yy, xx], nc, L2, yy, xx))
    if goal is None:
        return None
    # reconstruct
    path = []
    L, yy, xx = goal
    while True:
        path.append((L, yy, xx))
        p = parent[L][yy, xx]
        if p < 0: break
        pl = p // (GH * GW); rem = p % (GH * GW); py, px = rem // GW, rem % GW
        L, yy, xx = pl, py, px
    path.reverse()
    # split into per-layer runs + vias
    tracks = []
    vias = []
    run = [path[0]]
    def seg_ok(L, p0, p1):
        P = passable[L]
        d = math.hypot(p1[0]-p0[0], p1[1]-p0[1])
        n = max(2, int(d / 0.04) + 1)
        for k in range(n + 1):
            x = p0[0] + (p1[0]-p0[0]) * k / n
            y = p0[1] + (p1[1]-p0[1]) * k / n
            ccx, cyy = int(round(cx(x))), int(round(cy(y)))
            if not (0 <= ccx < GW and 0 <= cyy < GH) or not P[cyy, ccx]:
                return False
        return True
    def flush(run):
        if len(run) < 2: return
        L = run[0][0]
        pts = [(cellxy(x, y)) for (_, y, x) in run]
        # greedy line-of-sight shortcut (binary search farthest passable jump)
        simp = [pts[0]]
        i = 0
        while i < len(pts) - 1:
            lo, hi = i + 1, len(pts) - 1
            best = i + 1
            while lo <= hi:
                mid = (lo + hi) // 2
                if seg_ok(L, pts[i], pts[mid]):
                    best = mid; lo = mid + 1
                else:
                    hi = mid - 1
            simp.append(pts[best]); i = best
        # trim end segments fully covered by existing own copper (incl. pour):
        # drawing there is redundant and can break edge rules at pour borders
        def owncov(L, x, y):
            ccx, cyy = int(round(cx(x))), int(round(cy(y)))
            if not (0 <= ccx < GW and 0 <= cyy < GH): return False
            if B.owner[L][cyy, ccx] == code: return True
            return net in ('GND', 'V3_3') and B.zfill[L][cyy, ccx] == code
        def seg_own(L, p0, p1):
            for k in range(6):
                x = p0[0] + (p1[0]-p0[0]) * k / 5
                y = p0[1] + (p1[1]-p0[1]) * k / 5
                if not owncov(L, x, y): return False
            return True
        while len(simp) > 2 and seg_own(L, simp[0], simp[1]): simp.pop(0)
        while len(simp) > 2 and seg_own(L, simp[-2], simp[-1]): simp.pop()
        if len(simp) >= 2:
            tracks.append((L, simp))
    for i in range(1, len(path)):
        if path[i][0] != path[i-1][0]:
            vx, vy = cellxy(path[i][2], path[i][1])
            # if this column already has own copper on every layer (PTH barrel /
            # pour overlap), the layer change needs no new via
            if not all((B.owner[L][path[i][1], path[i][2]] == code) or
                       (net in ('GND', 'V3_3') and B.zfill[L][path[i][1], path[i][2]] == code)
                       for L in range(4)):
                vias.append((vx, vy))
            flush(run)
            run = [path[i]]
        else:
            run.append(path[i])
    flush(run)
    if not tracks and not vias:
        return None  # path lay entirely on existing own copper: no real link
    return tracks, vias

# ---------------- board edit helpers ----------------
def net_code(board, net):
    # robust: source the code from pads/tracks (GetNetInfo can return a dead
    # SwigPyObject after board edits in 7.0.11)
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetname() == net:
                return pad.GetNetCode()
    for t in board.GetTracks():
        if t.GetNetname() == net:
            return t.GetNetCode()
    ni = board.GetNetInfo()
    for code in range(ni.GetNetCount()):
        item = ni.GetNetItem(code)
        if item is not None and item.GetNetname() == net:
            return code
    raise KeyError(net)

def add_tracks(board, net, tracks, width=0.25):
    code = net_code(board, net)
    made = []
    for (L, pts) in tracks:
        for i in range(len(pts)-1):
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(pcbnew.VECTOR2I(nm(pts[i][0]), nm(pts[i][1])))
            t.SetEnd(pcbnew.VECTOR2I(nm(pts[i+1][0]), nm(pts[i+1][1])))
            t.SetWidth(nm(width))
            t.SetLayer(LAYERS[L])
            t.SetNetCode(code)
            board.Add(t); made.append(t)
    return made

def add_vias(board, net, vias):
    code = net_code(board, net)
    made = []
    for (x, y) in vias:
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I(nm(x), nm(y)))
        v.SetWidth(nm(VIA_DIA)); v.SetDrill(nm(VIA_DRILL))
        v.SetViaType(3)
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        v.SetNetCode(code)
        board.Add(v); made.append(v)
    return made

def ascii_map(B, net, x0, y0, x1, y1, layer, lab=None, compA=0, compB=0, step=2):
    code = B.netcode[net]
    lines = []
    xa, xb = int(cx(x0)), int(cx(x1)); ya, yb = int(cy(y0)), int(cy(y1))
    hdr = '     ' + ''.join([str(int((OX + c*PITCH)) % 10) if c % 2 == 0 else ' ' for c in range(xa, xb, step)])
    lines.append(hdr)
    for r in range(ya, yb, step):
        row = []
        for c in range(xa, xb, step):
            o = B.owner[layer][r, c]
            ch = '.'
            if B.hard[layer][r, c]: ch = 'X'
            elif lab is not None and lab[layer][r, c] == compA and compA: ch = 'A'
            elif lab is not None and lab[layer][r, c] == compB and compB: ch = 'B'
            elif o == code: ch = 'o'
            elif o != 0: ch = '#'
            elif B.zfill[layer][r, c] == code: ch = '+'
            elif B.zfill[layer][r, c] != 0: ch = '-'
            row.append(ch)
        lines.append('%5.1f %s' % (OY + r*PITCH, ''.join(row)))
    return '\n'.join(lines)
