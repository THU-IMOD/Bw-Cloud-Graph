#!/usr/bin/env bash
# Use the JDK signal-chaining library: Folly's shared-library initialization
# changes SIGPIPE, which must not replace HotSpot's signal handler.
set -euo pipefail
if [[ $# -lt 1 ]]; then
  echo 'Usage: bash tools/jni_java.sh /path/to/java [Java arguments...]' >&2
  exit 2
fi
java_bin="$1"
shift
java_home="$("$java_bin" -XshowSettings:properties -version 2>&1 | awk '/^[[:space:]]*java.home = / { sub(/^[^=]*= /, ""); print; exit }')"
case "$(uname -s)" in
  Darwin)
    jsig="$java_home/lib/libjsig.dylib"
    [[ -f "$jsig" ]] || { echo "Missing JDK signal library: $jsig" >&2; exit 1; }
    export DYLD_INSERT_LIBRARIES="$jsig${DYLD_INSERT_LIBRARIES:+:$DYLD_INSERT_LIBRARIES}"
    ;;
  Linux)
    jsig="$java_home/lib/libjsig.so"
    [[ -f "$jsig" ]] || { echo "Missing JDK signal library: $jsig" >&2; exit 1; }
    export LD_PRELOAD="$jsig${LD_PRELOAD:+:$LD_PRELOAD}"
    ;;
  *) echo 'The native Bw-Graph adapter currently targets macOS and Linux.' >&2; exit 1 ;;
esac
exec "$java_bin" "$@"
