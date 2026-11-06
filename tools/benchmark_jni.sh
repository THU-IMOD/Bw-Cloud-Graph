#!/usr/bin/env bash
# Benchmark the JNI point-request path, not graph insertion or graph analytics.
set -euo pipefail
build_dir="${1:-build-jni-release}"
out_dir="${2:-output/jni-bench-$(date +%Y%m%d-%H%M%S)}"
[[ -f CMakeLists.txt && -f "$build_dir/CMakeCache.txt" ]] || { echo 'Run from the repository root with a configured build.' >&2; exit 1; }
if ! grep -q '^CMAKE_BUILD_TYPE:STRING=Release$' "$build_dir/CMakeCache.txt"; then
  echo 'A Release build is required.' >&2; exit 1
fi
java_bin="$(sed -n 's/^Java_JAVA_EXECUTABLE:FILEPATH=//p' "$build_dir/CMakeCache.txt")"
[[ -x "$java_bin" ]] || { echo 'Build with --jni first.' >&2; exit 1; }
case "$(uname -s)" in Darwin) suffix=dylib ;; Linux) suffix=so ;; *) exit 1 ;; esac
library="$(cd "$build_dir" && pwd)/lib/libbwgraph_jni.$suffix"
[[ -f "$library" ]] || { echo "Missing $library" >&2; exit 1; }
mkdir -p "$(dirname "$out_dir")"
mkdir "$out_dir" # Never overwrite an earlier run.
{
  date -u
  uname -a
  if command -v sw_vers >/dev/null; then sw_vers; sysctl hw.ncpu hw.memsize machdep.cpu.brand_string || true; fi
  if command -v lscpu >/dev/null; then lscpu; fi
  git rev-parse HEAD
  git status --short
  "$java_bin" -version
  cmake --version
  grep -E 'CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER:|BW_GRAPH_BUILD_JNI:|BW_GRAPH_ENABLE_TRANSACTION:' "$build_dir/CMakeCache.txt"
  if command -v brew >/dev/null; then brew list --versions folly openjdk@17; fi
  shasum -a 256 src/graph_service.cc bindings/jni/bw_graph_jni.cc bindings/java/test/BwGraphJNIBench.java
} > "$out_dir/environment.txt" 2>&1
bash tools/jni_java.sh "$java_bin" "-Dbwgraph.jni.path=$library" \
  -cp "$build_dir/java/classes" BwGraphJNIBench > "$out_dir/samples.csv" 2> "$out_dir/stderr.log"
echo "JNI samples: $out_dir/samples.csv"
