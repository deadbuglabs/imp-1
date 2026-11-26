#!/bin/bash
# Push one v18a cell job's outputs to the sweep-results branch, live, as soon
# as the job finishes — so a watcher can read it without waiting for artifacts.
#   push-results-v18a.sh <cell> <run_number>
#
# Reads from $WORKDIR (default: $GITHUB_WORKSPACE's sibling workdir, or the
# WORKDIR env var; falls back to the current directory):
#   $WORKDIR/out/<CELL>.json
#       -> results/v18a/<run_number>/<CELL>.json
#   $WORKDIR/sweep-results/results/v18a/<CELL>-state.json
#       -> results/v18a/<CELL>-state.json
#   $WORKDIR/sweep-results/results/v18a/<CELL>/  (.ses + plan artifacts)
#       -> results/v18a/<CELL>/
#
# Adapted from tools/sweep/push-results.sh: same detached-worktree pattern,
# same bot identity, same fetch+rebase+push retries (parallel jobs race on
# this branch; each job writes its own unique paths, so rebase is clean).
# Differences from the original: fixed family v18a + namespace
# results/v18a/<run>/<CELL>.json (not results/<run>/<family>-<variant>.json),
# and the per-cell state file + artifacts dir are pushed alongside the result.
# The state file + artifacts are what the NEXT run resumes from — the board
# itself is never committed; it is rebuilt from pristine + sessions/plans.
#
# Exit 0 on success (or when there is nothing new to commit); exit 1 only
# when the push itself failed after retries. A missing result JSON is a
# hard error (exit 1) — run_cell.sh must have produced one.
set -u
CELL=${1:?cell S01..S13}; RUN_NUMBER=${2:?run_number}
WORKDIR=${WORKDIR:-$(pwd)}
JSON=$WORKDIR/out/$CELL.json
START_DIR=$(pwd)
[ -f "$JSON" ] || { echo "results file missing: $JSON"; exit 1; }
git config user.name 'github-actions[bot]'
git config user.email 'github-actions[bot]@users.noreply.github.com'
WT=/tmp/sweep-results-wt
rm -rf "$WT"
git fetch origin sweep-results 2>/dev/null || true
if git rev-parse --verify origin/sweep-results >/dev/null 2>&1; then
  git worktree add "$WT" sweep-results
else
  git worktree add --detach "$WT"
  git -C "$WT" checkout --orphan sweep-results
  git -C "$WT" rm -rf . 2>/dev/null || true
fi
mkdir -p "$WT/results/v18a/$RUN_NUMBER"
cp "$JSON" "$WT/results/v18a/$RUN_NUMBER/$CELL.json"
# State file + artifacts: mirror from the run_cell worktree if present.
# (run_cell.sh already mirrors them into $WORKDIR/sweep-results; if that
# worktree is absent, the local cell-artifacts dir is the fallback source.)
STATE_SRC=""
[ -f "$WORKDIR/sweep-results/results/v18a/$CELL-state.json" ] && STATE_SRC="$WORKDIR/sweep-results/results/v18a/$CELL-state.json"
[ -z "$STATE_SRC" ] && [ -f "$WORKDIR/cell-artifacts/$CELL-state.json" ] && STATE_SRC="$WORKDIR/cell-artifacts/$CELL-state.json"
ART_SRC=""
[ -d "$WORKDIR/sweep-results/results/v18a/$CELL" ] && ART_SRC="$WORKDIR/sweep-results/results/v18a/$CELL"
[ -z "$ART_SRC" ] && [ -d "$WORKDIR/cell-artifacts/$CELL" ] && ART_SRC="$WORKDIR/cell-artifacts/$CELL"
ADDED="results/v18a/$RUN_NUMBER/$CELL.json"
if [ -n "$STATE_SRC" ]; then
  cp "$STATE_SRC" "$WT/results/v18a/$CELL-state.json"
  ADDED="$ADDED results/v18a/$CELL-state.json"
fi
if [ -n "$ART_SRC" ]; then
  mkdir -p "$WT/results/v18a/$CELL"
  cp -r "$ART_SRC/." "$WT/results/v18a/$CELL/"
  ADDED="$ADDED results/v18a/$CELL/"
fi
cd "$WT"
# shellcheck disable=SC2086
git add $ADDED
git commit -m "sweep run $RUN_NUMBER: v18a/$CELL" || { echo 'nothing to commit'; cd "$START_DIR"; git worktree remove --force "$WT" 2>/dev/null; exit 0; }
for i in 1 2 3 4 5; do
  if git push origin sweep-results; then
    echo "pushed results/v18a/$RUN_NUMBER/$CELL.json + state/artifacts"
    cd "$START_DIR"
    git worktree remove --force "$WT"
    exit 0
  fi
  echo "push conflict, retry $i/5"
  git pull --rebase origin sweep-results || git rebase --abort
done
echo 'push failed after 5 retries'
cd "$START_DIR"
git worktree remove --force "$WT"
exit 1
