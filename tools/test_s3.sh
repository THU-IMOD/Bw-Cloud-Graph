#!/usr/bin/env bash
# Real S3 integration. Does not delete remote objects or run against an implicit production prefix.
set -euo pipefail
cd "$(dirname "$0")/.."
build="${1:?Usage: tools/test_s3.sh BUILD_DIR [OUTPUT_DIR]}"
output="${2:-output/s3-$(date +%Y%m%d-%H%M%S)}"
: "${S3_ENDPOINT:?Set a plain HTTP(S) URL}"
: "${S3_BUCKET:?Set S3_BUCKET}"
: "${S3_TEST_PREFIX:?Set an isolated S3_TEST_PREFIX, never a live graph prefix}"
: "${AWS_ACCESS_KEY_ID:?Set credentials in the environment}"
: "${AWS_SECRET_ACCESS_KEY:?Set credentials in the environment}"
export BWGRAPH_STORAGE_BACKEND=s3
export S3_PREFIX="$S3_TEST_PREFIX"
mkdir -p "$output"
jobs="${S3_CTEST_JOBS:-1}"
timeout="${S3_CTEST_TIMEOUT:-3600}"
[[ "$jobs" =~ ^[1-9][0-9]*$ && "$timeout" =~ ^[1-9][0-9]*$ ]] || {
  echo 'S3_CTEST_JOBS and S3_CTEST_TIMEOUT must be positive integers.' >&2; exit 2;
}
# Credential values and complete environment dumps are deliberately excluded.
printf 'branch=%s\ncommit=%s\nbuild=%s\nprefix=%s\nctest_jobs=%s\nctest_timeout=%s\n' \
  "$(git branch --show-current)" "$(git rev-parse HEAD)" "$build" "$S3_PREFIX" \
  "$jobs" "$timeout" > "$output/run.txt"
# Fail early if the endpoint or its conditional-write contract is unavailable.
# These cases also check direct-frame I/O, manifest bounds and remote-only reopen.
ctest --test-dir "$build" -R '^test_s3_storage$' --output-on-failure --timeout "$timeout" \
  > "$output/s3-preflight.log" 2>&1
ctest --test-dir "$build" -j "$jobs" -E '^test_s3_storage$' --output-on-failure \
  --timeout "$timeout" > "$output/ctest.log" 2>&1
for layout in id-based kaminpar-gorder louvain-gorder; do
  "$build/bw-test/multiscale_smo" --layout "$layout" > "$output/smo-$layout.log" 2>&1
  "$build/bw-test/multiscale_smo" --layout "$layout" --vertex-index art > "$output/smo-$layout-art.log" 2>&1
done
if rg -q '^BW_GRAPH_ENABLE_TRANSACTION:BOOL=ON' "$build/CMakeCache.txt"; then
  "$build/bw-test/multiscale_txn" > "$output/txn.log" 2>&1
  "$build/bw-test/multiscale_txn" --vertex-index art > "$output/txn-art.log" 2>&1
fi
printf 'S3 validation passed: %s\n' "$output"
