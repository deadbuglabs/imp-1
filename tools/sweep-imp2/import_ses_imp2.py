#!/usr/bin/env python3
"""Import a Freerouting .ses into an IMP-2 board.

Usage: python3 import_ses_imp2.py <board.kicad_pcb> <session.ses>

Parameterized copy of hidden_files/imp2_work/import_ses2.py (the importer
proven for this exact board: Freerouting -mp 3 SES, 1384 wire segments +
179 vias, 0 skipped nets). pcbnew's ImportSpecctraSES needs the GUI's
current board, which is None standalone — so the SES is parsed directly.
Coordinate transform (verified vs R17): board_mm = (dsn_x*1e-4, -dsn_y*1e-4).
Via diameter/drill are parsed from the via padstack name in the SES
(e.g. Via[0-3]_600:300_um -> 0.6/0.3, Via[0-3]_500:300_um -> 0.5/0.3),
falling back to 0.6/0.3 when the name does not parse — so the finevia
variant's narrower vias survive the import instead of being silently
re-inflated to 0.6 mm.
"""
import pcbnew, re, sys

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
        elif c == "'":
            toks.append("'"); i += 1
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

VIA_RE = re.compile(r'(\d+):(\d+)_um')

def via_size(name):
    m = VIA_RE.search(name or '')
    if m:
        return float(m.group(1)) / 1000.0, float(m.group(2)) / 1000.0
    return 0.6, 0.3

def main():
    board_path, ses_path = sys.argv[1], sys.argv[2]
    ses = open(ses_path).read()
    tree = parse(tokenize(ses))

    def find_all(node, head):
        out = []
        if isinstance(node, list) and node and node[0] == head: out.append(node)
        if isinstance(node, list):
            for c in node:
                out += find_all(c, head)
        return out

    b = pcbnew.LoadBoard(board_path)
    print('loaded fps:', len(list(b.GetFootprints())), flush=True)
    netobjs = {}
    for ni in b.GetNetsByName().values():
        netobjs[ni.GetNetname()] = ni

    nwires = nvias = 0
    skipped_nets = []
    routes = find_all(tree, 'network_out')[0]
    for netnode in routes[1:]:
        if not (isinstance(netnode, list) and netnode and netnode[0] == 'net'): continue
        name = netnode[1]
        ni = netobjs.get(name)
        if ni is None:
            skipped_nets.append(name); continue
        for item in netnode[2:]:
            if not isinstance(item, list) or not item: continue
            if item[0] == 'wire':
                path = item[1]
                layer = b.GetLayerID(path[1]); width = mm(path[2])
                pts = [(mm(path[i]), -mm(path[i+1])) for i in range(3, len(path)-1, 2)]
                for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
                    t = pcbnew.PCB_TRACK(b)
                    t.SetStart(pcbnew.VECTOR2I_MM(x1, y1))
                    t.SetEnd(pcbnew.VECTOR2I_MM(x2, y2))
                    t.SetWidth(pcbnew.FromMM(width))
                    t.SetLayer(layer); t.SetNet(ni)
                    b.Add(t); nwires += 1
            elif item[0] == 'via':
                dia, drill = via_size(item[1] if len(item) > 1 else '')
                x, y = mm(item[2]), -mm(item[3])
                v = pcbnew.PCB_VIA(b)
                v.SetPosition(pcbnew.VECTOR2I_MM(x, y))
                v.SetDrill(pcbnew.FromMM(drill))
                v.SetWidth(pcbnew.FromMM(dia))
                v.SetLayerPair(b.GetLayerID('F.Cu'), b.GetLayerID('B.Cu'))
                v.SetNet(ni)
                b.Add(v); nvias += 1
    print('wires added:', nwires, 'vias added:', nvias, flush=True)
    print('skipped nets (not on board):', skipped_nets, flush=True)
    b.BuildConnectivity()
    print('unconnected after import:', b.GetConnectivity().GetUnconnectedCount(True), flush=True)
    print('tracks:', len(list(b.GetTracks())), flush=True)
    pcbnew.SaveBoard(board_path, b)
    print('saved routed board', flush=True)

if __name__ == '__main__':
    main()
