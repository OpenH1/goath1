#!/bin/sh
# Copies tools/rt_truth to the 5900X and runs `python -m rt_truth <args>` there, in its own venv.
# Usage: tools/rt_truth/run_5900x.sh render ~/goat-rig/rt2-scenes/c20_corner
set -e
HOST=${RT_TRUTH_HOST:-5900x}
TARGET=$(awk -v h="$HOST" '$1 == h { print $2 }' ~/.config/goat-rig/hosts 2>/dev/null)
TARGET=${TARGET:-$HOST.fabric}
HERE=$(cd "$(dirname "$0")" && pwd)
CODE=${RT_TRUTH_CODE:-goat-rig/rt2-truth-code}  # the copy's folder on the rig (one per helper)
ssh "$TARGET" "mkdir -p $CODE/rt_truth"
rsync -a --delete --exclude __pycache__ "$HERE/" "$TARGET:$CODE/rt_truth/"
ssh "$TARGET" "bash -lc 'cd ~/$CODE && PYTHONUNBUFFERED=1 RT_TRUTH_BUMP_SIGN=${RT_TRUTH_BUMP_SIGN:-1,1} ~/goat-rig/rt2-truth-venv/bin/python -m rt_truth $*'"
