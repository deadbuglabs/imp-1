"""Live snapshot watcher for A* sweep variants.

Watches an A* driver's cumulative plan file (checkpointed after every
link, same schema as route17.py's plan: add_tracks [{net, layer, pts, w}],
add_vias [{net, at, dia, drill}], delete/delete_vias, codes) and emits one
delta snapshot per plan growth event into a snapshot directory, batching
git pushes to the sweep-results branch.

  snap_watch.py --plan <plan.json> --snapdir <dir> --family v18a \
    --variant astar-0.05 --run-number 12 --run-id 123 \
    [--progress <progress.jsonl>] [--drc <drc.json>] \
    [--poll-secs 5] [--push-every-snaps 10] [--push-every-secs 30]
  snap_watch.py --flush-only <same args...>   # one-shot emit + push, then exit

Contract with the A* driver (see tools/sweep/README.md):
  - The driver writes its cumulative plan to --plan, checkpointed after
    every link (atomic replace preferred; a torn read is skipped, never
    fatal).
  - The driver may append progress lines to --progress (JSONL): an optional
    first line {"event": "start", "nets_total": N} and one line per finished
    net {"net": NAME, "unified": true|false, "links": n}.
  - The driver may publish stage DRC counts to --drc (parse_drc.py output);
    the freshest file is forwarded verbatim into snapshots.

Snapshot schema imp1-sweep-snap/1 and index imp1-sweep-snap-index/1 are
documented in tools/sweep/README.md. Freerouting variants never run this
watcher (replay only). The watcher never fails the job: every cycle is
guarded, and a failed push is logged and retried at the next batch.
SIGTERM/SIGINT triggers a final emit + push, then a clean exit 0.
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone

SNAP_SCHEMA = 'imp1-sweep-snap/1'
INDEX_SCHEMA = 'imp1-sweep-snap-index/1'


def utcnow():
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def log(*a):
    print('[snap_watch %s]' % utcnow(), *a, flush=True)


def load_json_quiet(path):
    """Return parsed JSON or None (missing file / torn write / bad JSON)."""
    try:
        with open(path) as fh:
            return json.load(fh)
    except Exception:
        return None


def atomic_write(path, obj):
    tmp = path + '.tmp-%d' % os.getpid()
    with open(tmp, 'w') as fh:
        json.dump(obj, fh, indent=1)
    os.replace(tmp, path)


class Watcher:
    def __init__(self, args):
        self.a = args
        self.root = os.getcwd()
        self.seq = 0
        self.pushed_seq = 0
        self.snaps_since_push = 0
        self.last_push_t = time.monotonic()
        self.last_track = 0
        self.last_via = 0
        self.last_del = 0
        self.last_delvia = 0
        self.last_drc_mtime = 0.0
        self.drc = None
        self.drc_source = None
        self.stop = False
        self.state_path = os.path.join(args.snapdir, 'state.json')
        self._load_state()

    def _load_state(self):
        st = load_json_quiet(self.state_path)
        if not st:
            return
        self.seq = st.get('seq', 0)
        self.pushed_seq = st.get('pushed_seq', 0)
        self.last_track = st.get('last_track', 0)
        self.last_via = st.get('last_via', 0)
        self.last_del = st.get('last_del', 0)
        self.last_delvia = st.get('last_delvia', 0)
        self.last_drc_mtime = st.get('last_drc_mtime', 0.0)

    def _save_state(self):
        try:
            atomic_write(self.state_path, {
                'seq': self.seq, 'pushed_seq': self.pushed_seq,
                'last_track': self.last_track, 'last_via': self.last_via,
                'last_del': self.last_del, 'last_delvia': self.last_delvia,
                'last_drc_mtime': self.last_drc_mtime,
            })
        except Exception as e:
            log('state save failed (non-fatal):', e)

    def read_progress(self):
        """Return (progress dict, last finished net, links_landed)."""
        progress = {'nets_unified': None, 'nets_sealed': None,
                    'nets_remaining': None, 'nets_total': None}
        last_net = None
        last_unified = None
        links = 0
        by_net = {}
        total = None
        try:
            with open(self.a.progress) as fh:
                for line in fh:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        rec = json.loads(line)
                    except Exception:
                        continue
                    if rec.get('event') == 'start':
                        total = rec.get('nets_total')
                        continue
                    net = rec.get('net')
                    if not net:
                        continue
                    by_net[net] = rec
        except FileNotFoundError:
            return progress, None, None, None
        except Exception as e:
            log('progress read failed (non-fatal):', e)
            return progress, None, None, None
        if not by_net:
            if total is not None:
                progress['nets_total'] = total
                progress['nets_remaining'] = total
            return progress, None, None, None
        unified = sum(1 for r in by_net.values() if r.get('unified') is True)
        sealed = sum(1 for r in by_net.values() if r.get('unified') is False)
        links = sum(int(r.get('links') or 0) for r in by_net.values())
        # last finished net = last record in file order
        last = list(by_net.values())[-1]
        last_net = last.get('net')
        last_unified = last.get('unified')
        progress['nets_unified'] = unified
        progress['nets_sealed'] = sealed
        progress['nets_total'] = total
        progress['nets_remaining'] = (total - unified - sealed) if total is not None else None
        return progress, last_net, last_unified, links

    def read_drc(self):
        """Forward the driver's freshest stage DRC, if any. Returns True if updated."""
        if not self.a.drc:
            return False
        try:
            mt = os.path.getmtime(self.a.drc)
        except OSError:
            return False
        if mt <= self.last_drc_mtime:
            return False
        d = load_json_quiet(self.a.drc)
        if d is None:
            return False
        self.drc = d
        self.drc_source = 'driver-stage'
        self.last_drc_mtime = mt
        return True

    def emit(self):
        """Emit one delta snapshot if the plan grew or a fresh DRC landed."""
        plan = load_json_quiet(self.a.plan)
        if plan is None:
            return False
        tracks = plan.get('add_tracks') or []
        vias = plan.get('add_vias') or []
        dels = plan.get('delete') or []
        delvias = plan.get('delete_vias') or []
        grew = (len(tracks) != self.last_track or len(vias) != self.last_via
                or len(dels) != self.last_del or len(delvias) != self.last_delvia)
        drc_fresh = self.read_drc()
        if not grew and not drc_fresh:
            return False
        new_tracks = tracks[self.last_track:]
        new_vias = vias[self.last_via:]
        new_dels = dels[self.last_del:]
        new_delvias = delvias[self.last_delvia:]
        self.last_track, self.last_via = len(tracks), len(vias)
        self.last_del, self.last_delvia = len(dels), len(delvias)
        progress, last_net, last_unified, links = self.read_progress()
        # net just worked on: progress file wins, else last new geometry's net
        net = last_net
        unified = last_unified
        if net is None and new_tracks:
            net = new_tracks[-1].get('net')
        if net is None and new_vias:
            net = new_vias[-1].get('net')
        unconn_est = None
        if progress.get('nets_total') is not None and progress.get('nets_unified') is not None:
            # every net not yet unified (sealed + remaining) still shows as
            # unconnected; this is the router's own count, not a DRC count
            unconn_est = progress['nets_total'] - progress['nets_unified']
        self.seq += 1
        snap = {
            'schema': SNAP_SCHEMA,
            'run_number': int(self.a.run_number),
            'run_id': str(self.a.run_id),
            'family': self.a.family,
            'variant': self.a.variant,
            'seq': self.seq,
            't_utc': utcnow(),
            'net': net,
            'net_unified': unified,
            'links_landed': links,
            'tracks_new': new_tracks,
            'vias_new': new_vias,
            'delete_new': new_dels,
            'delete_vias_new': new_delvias,
            'plan_tracks': len(tracks),
            'plan_vias': len(vias),
            'progress': progress,
            'unconnected_estimate': unconn_est,
            'drc': self.drc,
            'drc_source': self.drc_source,
        }
        os.makedirs(self.a.snapdir, exist_ok=True)
        atomic_write(os.path.join(self.a.snapdir, 'snap-%04d.json' % self.seq), snap)
        self.snaps_since_push += 1
        self.write_index(snap)
        self._save_state()
        log('snap %04d: net=%s unified=%s +%d tracks +%d vias (plan %d/%d)'
            % (self.seq, net, unified, len(new_tracks), len(new_vias),
               len(tracks), len(vias)))
        return True

    def write_index(self, latest_snap=None):
        try:
            snaps = sorted(f for f in os.listdir(self.a.snapdir)
                           if f.startswith('snap-') and f.endswith('.json'))
        except OSError:
            snaps = []
        progress, _, _, links = self.read_progress()
        unconn_est = None
        if progress.get('nets_total') is not None and progress.get('nets_unified') is not None:
            unconn_est = progress['nets_total'] - progress['nets_unified']
        idx = {
            'schema': INDEX_SCHEMA,
            'run_number': int(self.a.run_number),
            'run_id': str(self.a.run_id),
            'family': self.a.family,
            'variant': self.a.variant,
            'latest_seq': self.seq,
            'pushed_seq': self.pushed_seq,
            'snap_count': len(snaps),
            'snaps': snaps,
            'links_landed': links,
            'progress': progress,
            'unconnected_estimate': unconn_est,
            'drc': self.drc,
            'drc_source': self.drc_source,
            'updated_utc': utcnow(),
        }
        os.makedirs(self.a.snapdir, exist_ok=True)
        try:
            atomic_write(os.path.join(self.a.snapdir, 'index.json'), idx)
        except Exception as e:
            log('index write failed (non-fatal):', e)

    def push(self):
        """Push new snapshots + index. Returns True on success."""
        if self.pushed_seq >= self.seq:
            return True
        target = self.seq
        prev_pushed = self.pushed_seq
        # claim the push in the index first, so the index the push carries
        # is truthful about what it contains; roll back on failure
        self.pushed_seq = target
        self.write_index()
        self._save_state()
        cmd = ['bash', os.path.join('tools', 'sweep', 'push-snaps.sh'),
               self.a.snapdir, str(self.a.run_number),
               self.a.family, self.a.variant]
        try:
            r = subprocess.run(cmd, cwd=self.root, capture_output=True,
                               text=True, timeout=300)
        except Exception as e:
            log('push-snaps failed to run:', e)
            self.pushed_seq = prev_pushed
            self.write_index()
            self._save_state()
            return False
        if r.returncode != 0:
            log('push-snaps failed rc=%d: %s' % (r.returncode,
                (r.stdout + r.stderr).strip()[-500:]))
            self.pushed_seq = prev_pushed
            self.write_index()
            self._save_state()
            return False
        self.snaps_since_push = 0
        self.last_push_t = time.monotonic()
        self._save_state()
        log('pushed through seq %04d' % self.seq)
        return True

    def maybe_push(self):
        if self.seq <= self.pushed_seq:
            return
        due_count = self.snaps_since_push >= self.a.push_every_snaps
        due_time = (time.monotonic() - self.last_push_t) >= self.a.push_every_secs
        if due_count or due_time:
            self.push()

    def run(self):
        log('watching plan=%s snapdir=%s poll=%ss push every %d snaps or %ss'
            % (self.a.plan, self.a.snapdir, self.a.poll_secs,
               self.a.push_every_snaps, self.a.push_every_secs))
        while not self.stop:
            try:
                self.emit()
            except Exception as e:
                log('emit cycle failed (non-fatal):', e)
            try:
                self.maybe_push()
            except Exception as e:
                log('push cycle failed (non-fatal):', e)
            # sleep in small slices so SIGTERM lands promptly
            deadline = time.monotonic() + self.a.poll_secs
            while not self.stop and time.monotonic() < deadline:
                time.sleep(0.5)
        log('stopping: final emit + push')
        try:
            self.emit()
        except Exception as e:
            log('final emit failed (non-fatal):', e)
        try:
            self.push()
        except Exception as e:
            log('final push failed (non-fatal):', e)
        log('done, pushed through seq %04d of %04d' % (self.pushed_seq, self.seq))
        return 0

    def flush_only(self):
        log('flush-only: emit + push once')
        try:
            self.emit()
        except Exception as e:
            log('flush emit failed (non-fatal):', e)
        ok = self.push()
        log('flush-only done, pushed=%s seq=%04d' % (ok, self.seq))
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--plan', required=True)
    ap.add_argument('--snapdir', required=True)
    ap.add_argument('--family', required=True)
    ap.add_argument('--variant', required=True)
    ap.add_argument('--run-number', required=True)
    ap.add_argument('--run-id', required=True)
    ap.add_argument('--progress', default=None)
    ap.add_argument('--drc', default=None)
    ap.add_argument('--poll-secs', type=float, default=5)
    ap.add_argument('--push-every-snaps', type=int, default=10)
    ap.add_argument('--push-every-secs', type=float, default=30)
    ap.add_argument('--flush-only', action='store_true')
    a = ap.parse_args()

    w = Watcher(a)
    if a.flush_only:
        return w.flush_only()

    def _term(signum, frame):
        w.stop = True
    signal.signal(signal.SIGTERM, _term)
    signal.signal(signal.SIGINT, _term)
    return w.run()


if __name__ == '__main__':
    sys.exit(main())
