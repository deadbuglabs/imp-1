#!/bin/bash
# Push one job's live snapshots to the sweep-results branch.
#   push-snaps.sh <snapdir> <run_number> <family> <variant>
# Pushes <snapdir>/*.json (snap-NNNN.json + index.json) to
#   results/<run_number>/<family>-<variant>/snaps/
# on branch sweep-results, so a watcher can poll them live.
# Reuses one persistent worktree (/tmp/sweep-snaps-wt) across calls within a
# job — distinct from push-results.sh's /tmp/sweep-results-wt, which the
# final results push uses after the watcher has stopped. Retries
# fetch+rebase+push up to 5 times; parallel jobs write disjoint paths, so
# rebase is clean. Exit 0 when there is nothing new to push.
set -u
SNAPDIR=$1; RUN_NUMBER=$2; FAMILY=$3; VARIANT=$4
START_DIR=$(pwd)
[ -d "$SNAPDIR" ] || { echo "snapdir missing: $SNAPDIR"; exit 1; }
git config user.name 'github-actions[bot]'
git config user.email 'github-actions[bot]@users.noreply.github.com'
WT=/tmp/sweep-snaps-wt
if [ ! -d "$WT/.git" ]; then
  rm -rf "$WT"
  git fetch origin sweep-results 2>/dev/null || true
  if git rev-parse --verify origin/sweep-results >/dev/null 2>&1; then
    git worktree add "$WT" sweep-results 2>/dev/null || git worktree add --force "$WT" sweep-results
  else
    git worktree add --detach "$WT"
    git -C "$WT" checkout --orphan sweep-results
    git -C "$WT" rm -rf . 2>/dev/null || true
  fi
fi
# refresh against other jobs' pushes (disjoint paths; rebase stays clean)
git -C "$WT" fetch origin sweep-results 2>/dev/null || true
if git -C "$WT" rev-parse --verify origin/sweep-results >/dev/null 2>&1; then
  git -C "$WT" pull --rebase origin sweep-results 2>/dev/null || git -C "$WT" rebase --abort 2>/dev/null || true
fi
DEST="$WT/results/$RUN_NUMBER/${FAMILY}-${VARIANT}/snaps"
mkdir -p "$DEST"
cp "$SNAPDIR"/snap-*.json "$DEST/" 2>/dev/null || true
cp "$SNAPDIR"/index.json "$DEST/" 2>/dev/null || true
cd "$WT"
git add "results/$RUN_NUMBER/${FAMILY}-${VARIANT}/snaps"
if git diff --cached --quiet; then
  echo 'nothing new to push'
  cd "$START_DIR"
  exit 0
fi
git commit -m "sweep run $RUN_NUMBER: $FAMILY/$VARIANT snaps" || { echo 'commit failed'; cd "$START_DIR"; exit 1; }
for i in 1 2 3 4 5; do
  if git push origin sweep-results; then
    echo "pushed results/$RUN_NUMBER/${FAMILY}-${VARIANT}/snaps"
    cd "$START_DIR"
    exit 0
  fi
  echo "push conflict, retry $i/5"
  git pull --rebase origin sweep-results || git rebase --abort
done
echo 'push failed after 5 retries'
cd "$START_DIR"
exit 1
