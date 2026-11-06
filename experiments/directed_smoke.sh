#!/usr/bin/env bash
# Native directed format: isolated import, verified BYO BFS/WCC/20-round PageRank.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
build=${1:-build-directed-release}
out=${2:-output/directed-perf-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$(dirname "$out")"
mkdir "$out"
out=$(cd "$out" && pwd)
{
  git rev-parse HEAD
  git branch --show-current
  uname -a
  sysctl -n machdep.cpu.brand_string hw.memsize 2>/dev/null || true
  clang++ --version
} > "$out/environment.txt"
cp "$build/CMakeCache.txt" "$out/CMakeCache.txt"
brew list --versions > "$out/brew-versions.txt"
git diff HEAD > "$out/source.patch"
git ls-files --others --exclude-standard -z | tar --null -T - -czf "$out/untracked-source.tar.gz"
for dataset in rn yt; do
  test -f "data/$dataset.graph"
  shasum -a 256 "data/$dataset.graph" >> "$out/datasets.sha256"
  python3 - "$out" "$dataset" <<'PY'
from pathlib import Path
import sys
out, dataset = Path(sys.argv[1]), sys.argv[2]
s = Path('config/example.yaml').read_text()
s = s.replace('engine: legacy', 'engine: multiscale').replace('name: example', f'name: {dataset}')
s = s.replace('./workspace/example/', str(out / dataset) + '/')
s = s.replace('rebuild: false', 'rebuild: true').replace('redo: false', 'redo: true')
s = s.replace('full_giant_warmup: true', 'full_giant_warmup: false')
(out / f'{dataset}-import.yaml').write_text(s)
s = s.replace('rebuild: true', 'rebuild: false').replace('redo: true', 'redo: false')
(out / f'{dataset}.yaml').write_text(s)
PY
  "$build/bin/byo" "$out/$dataset-import.yaml" map 0 16 > "$out/$dataset-import.log" 2>&1
  for algo in bfs cc pagerank; do
    "$build/bw-test/byo_bench" "$repo" "$out/$dataset.yaml" 64 3 16 byo \
      "$out/$dataset-$algo.json" "$algo" 20 > "$out/$dataset-$algo.log" 2>&1
    cat "$out/$dataset-$algo.log"
  done
done
python3 - "$out" <<'PY'
from pathlib import Path
import json, statistics, sys
out = Path(sys.argv[1])
lines = ['dataset,algorithm,threads,CSR_MiB,samples,median_ms,min_ms,max_ms']
for dataset in ('rn', 'yt'):
    for algo in ('bfs', 'cc', 'pagerank'):
        j = json.loads((out / f'{dataset}-{algo}.json').read_text())
        times = [r['seconds'] * 1000 for r in j['rounds']]
        lines.append(f'{dataset},{algo},16,64,{len(times)},{statistics.median(times):.3f},{min(times):.3f},{max(times):.3f}')
(out / 'summary.csv').write_text('\n'.join(lines) + '\n')
print('\n'.join(lines))
PY
