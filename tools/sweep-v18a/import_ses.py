"""v18a sweep: manual SES import (pcbnew.ImportSpecctraSES needs the GUI's
current board, which is None headless — parse the SES directly, per imp2's
import_ses2.py, verified transform board_mm = (dsn_x*1e-4, -dsn_y*1e-4)).
PORT NOTE (tools/sweep-v18a, GitHub Actions port): identical to the local
v18a-sweep_work/import_ses.py except the sys.path insert, which is now
repo-relative (this file's ../v18a dir, where blib.py lives) instead of the
absolute workspace path. No absolute paths remain.
Usage: import_ses.py <board> <ses> ; saves board in place."""
import sys, os, re
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'v18a')))
import blib, pcbnew

board_path, ses_path = sys.argv[1], sys.argv[2]

def tokenize(s):
    toks, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if c in ' \t\r\n': i += 1
        elif c == '(': toks.append('('); i += 1
        elif c == ')': toks.append(')'); i += 1
        elif c == '"':
            j = s.find('"', i+1)
            toks.append(s[i+1:j]); i = j+1
        elif c == "'": toks.append("'"); i += 1
        else:
            j = i
            while j < n and s[j] not in ' \t\r\n()"': j += 1
            toks.append(s[i:j]); i = j
    return toks

def parse(toks):
    stack, cur = [], []
    for t in toks:
        if t == '(': stack.append(cur); cur = []
        elif t == ')':
            node = cur; cur = stack.pop(); cur.append(node)
        else: cur.append(t)
    return cur

def mm(v): return float(v) * 1e-4

ses = open(ses_path).read()
tree = parse(tokenize(ses))

def find_all(node, head):
    out = []
    if isinstance(node, list) and node and node[0] == head: out.append(node)
    if isinstance(node, list):
        for c in node:
            out += find_all(c, head)
    return out

b = blib.load(board_path)
print('loaded fps:', len(list(b.GetFootprints())), flush=True)
netobjs = {}
for ni in b.GetNetsByName().values():
    netobjs[ni.GetNetname()] = ni

nwires = nvias = 0
skipped_nets = []
routes = find_all(tree, 'network_out')
if not routes:
    print('ERROR: no network_out in SES'); sys.exit(1)
for netnode in routes[0][1:]:
    if not (isinstance(netnode, list) and netnode and netnode[0] == 'net'): continue
    name = netnode[1]
    ni = netobjs.get(name)
    if ni is None:
        skipped_nets.append(name); continue
    for item in netnode[2:]:
        if not isinstance(item, list) or not item: continue
        if item[0] == 'wire':
            path = item[1]
            try:
                layer = b.GetLayerID(path[1]); width = mm(path[2])
            except Exception as e:
                print('wire layer/width fail', name, path[:3], e); continue
            pts = [(mm(path[i]), -mm(path[i+1])) for i in range(3, len(path)-1, 2)]
            for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
                t = pcbnew.PCB_TRACK(b)
                t.SetStart(pcbnew.VECTOR2I_MM(x1, y1))
                t.SetEnd(pcbnew.VECTOR2I_MM(x2, y2))
                t.SetWidth(pcbnew.FromMM(width))
                t.SetLayer(layer); t.SetNet(ni)
                b.Add(t); nwires += 1
        elif item[0] == 'via':
            x, y = mm(item[2]), -mm(item[3])
            v = pcbnew.PCB_VIA(b)
            v.SetPosition(pcbnew.VECTOR2I_MM(x, y))
            v.SetDrill(pcbnew.FromMM(0.3))
            v.SetWidth(pcbnew.FromMM(0.6))
            v.SetLayerPair(b.GetLayerID('F.Cu'), b.GetLayerID('B.Cu'))
            v.SetNet(ni)
            b.Add(v); nvias += 1
print('wires added:', nwires, 'vias added:', nvias, flush=True)
print('skipped nets (not on board):', skipped_nets, flush=True)
b.BuildConnectivity()
print('unconnected after import (connectivity count):', b.GetConnectivity().GetUnconnectedCount(True), flush=True)
print('tracks:', len(list(b.GetTracks())), flush=True)
blib.save(b, board_path)
print('saved routed board', flush=True)
