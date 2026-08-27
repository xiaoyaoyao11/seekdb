#!/usr/bin/env bash
# Run one mysqltest slice directly against seekdb.
# Required env: GITHUB_WORKSPACE, SEEKDB_TASK_DIR, SEEKDB_BINARY, SLICE_IDX, SLICES
# Optional: SEEKDB_RUNTIME_DIR, MYSQLTEST_PORT
set -euo pipefail

WORKSPACE="${GITHUB_WORKSPACE:?}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TASK_DIR="${SEEKDB_TASK_DIR:?}"
SLICE_IDX="${SLICE_IDX:-0}"
SLICES="${SLICES:-4}"
RUNTIME_DIR="${SEEKDB_RUNTIME_DIR:-$WORKSPACE/.seekdb_runtime}"
SEEKDB_BINARY="${SEEKDB_BINARY:?}"
CLIENT_ROOT="$RUNTIME_DIR/obclient"
PORT="${MYSQLTEST_PORT:-$((5000 + SLICE_IDX * 100))}"

export GITHUB_WORKSPACE="$WORKSPACE"
export SEEKDB_TASK_DIR="${SEEKDB_TASK_DIR:?}"
export SLICE_IDX="${SLICE_IDX:-0}"
export SLICES="${SLICES:-4}"
export BRANCH="${BRANCH:-master}"


# Copy compile artifacts from task dir to workspace if running in container
for f in observer.zst; do
  if [[ -f "$SEEKDB_TASK_DIR/$f" ]] && [[ ! -f "$WORKSPACE/$f" ]]; then
    cp -f "$SEEKDB_TASK_DIR/$f" "$WORKSPACE/" || true
  fi
done

if [[ -f "$SCRIPTS_DIR/mysqltest_for_seekdb.sh" ]]; then
  bash "$SCRIPTS_DIR/mysqltest_for_seekdb.sh" "$@"
else
  echo "[mysqltest_slice.sh] No mysqltest_for_seekdb.sh, skip slice $SLICE_IDX."
fi
