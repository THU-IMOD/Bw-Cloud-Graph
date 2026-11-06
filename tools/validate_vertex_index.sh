#!/usr/bin/env bash
# Run the existing MTR correctness suites against both runtime MVVI backends.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
build=${1:-build}
logs=${2:-output/vertex-index-validation-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$logs"
for backend in array art; do
  for suite in read_neighbor_clone page_delta_concurrency vertex_version_concurrency; do
    "$build/bw-test/$suite" "$repo" --vertex-index "$backend" > "$logs/$suite-$backend.log" 2>&1
    echo "[pass] $suite $backend"
  done
  for layout in id-based kaminpar-gorder louvain-gorder; do
    "$build/bw-test/multiscale_smo" --layout "$layout" --vertex-index "$backend" \
      > "$logs/multiscale_smo-$layout-$backend.log" 2>&1
    echo "[pass] multiscale_smo $layout $backend"
  done
  if rg -q '^BW_GRAPH_ENABLE_TRANSACTION:BOOL=ON$' "$build/CMakeCache.txt"; then
    "$build/bw-test/txn_snapshot" --vertex-index "$backend" > "$logs/txn_snapshot-$backend.log" 2>&1
    echo "[pass] txn_snapshot $backend"
    for layout in id-based kaminpar-gorder louvain-gorder; do
      "$build/bw-test/multiscale_txn" "$repo" --layout "$layout" --vertex-index "$backend" \
        > "$logs/multiscale_txn-$layout-$backend.log" 2>&1
      echo "[pass] multiscale_txn $layout $backend"
    done
  fi
done
