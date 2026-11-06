# Bw-Cloud-Graph

Build and run the graph storage engine with a local or S3-compatible backend.

The cloud build additionally requires **libcurl >= 7.75** and **OpenSSL**.
On macOS, install them with `brew install curl openssl@3`. If CMake selects an
older curl, supply the installed curl prefix through `CMAKE_PREFIX_PATH`.

## Prerequisites

Run the commands below from the repository root. Install dependencies using
[AGENTS.md, sections 2–3](AGENTS.md#3-setting-up-a-fresh-environment): C++20,
CMake, Homebrew/Linuxbrew folly, source-built RocksDB and oneTBB, and the
preserved KaMinPar sources. ART and Louvain are built as project submodules.
JNI requires a Java 17+ JDK; set `JAVA_HOME` to its installation directory.

```bash
git submodule update --init --recursive
# macOS with Homebrew OpenJDK 17:
export JAVA_HOME="$(brew --prefix openjdk@17)/libexec/openjdk.jdk/Contents/Home"
# Linux: set JAVA_HOME to your installed JDK instead.
```

## Build and test

Use a fresh build directory after moving the checkout or changing platforms.
The build script also runs CTest. Omit `--jni` for a C++-only build.

```bash
BWGRAPH_STORAGE_BACKEND=local bash tools/build.sh \
  --build-dir build-release -t release --jni -j 8

# Optional transaction-enabled build and tests:
BWGRAPH_STORAGE_BACKEND=local bash tools/build.sh \
  --build-dir build-txn -t debug --jni --txn -j 8
```

Pass `--txn` on each invocation for the transaction build; script options do not
persist between invocations. See `bash tools/build.sh --help` for other options.

## Run the included graph

This uses `data/example.graph` and creates an isolated multiscale database.

```bash
export BWGRAPH_STORAGE_BACKEND=local
run_dir=$(mktemp -d "$PWD/build-release/smoke.XXXXXX")
sed -e "s|./workspace/example/|$run_dir/db/|g" \
    -e 's|engine: legacy|engine: multiscale|' \
    -e 's|rebuild: false|rebuild: true|' \
    -e 's|redo: false|redo: true|' \
    config/example.yaml > "$run_dir/create.yaml"
./build-release/bin/bfs "$run_dir/create.yaml" --algo map --benchmark

# Reopen the same database without rebuilding it:
sed -e 's|rebuild: true|rebuild: false|' \
    -e 's|redo: true|redo: false|' \
    "$run_dir/create.yaml" > "$run_dir/open.yaml"
./build-release/bin/bfs "$run_dir/open.yaml" --algo map --benchmark
./build-release/bin/wcc "$run_dir/open.yaml" --algo map --benchmark
./build-release/bin/pagerank "$run_dir/open.yaml" --algo map --benchmark
```

For another input, place `<name>.graph` under `data/`, set `graph.name` in a copy
of `config/example.yaml`, and assign new storage paths. Keep existing databases
and their configurations separate.

## Java / MonacGraph

The JNI build produces `build-release/java/bwgraph-jni.jar` and
`build-release/lib/libbwgraph_jni.dylib` (macOS) or `libbwgraph_jni.so` (Linux).
Run the JVM integration test with:

```bash
BWGRAPH_STORAGE_BACKEND=local ctest --test-dir build-release \
  -R '^test_jni$' --output-on-failure
```

In a MonacGraph checkout with the Bw-Graph adapter, select
`storage.backend=bw-graph` in its configuration and set `BWGRAPH_SOURCE` to this
checkout's absolute path. Its launcher builds/packages JNI and sets up JVM signal
chaining. Java callers must close graph handles with `closeDB()`.

## S3 / MinIO

Create the bucket first and provide credentials through the process environment;
do not put credentials in source files or committed configurations.

```bash
export BWGRAPH_STORAGE_BACKEND=s3
export S3_ENDPOINT='http://your-minio-host:9000'
export S3_BUCKET='your-bucket'
export S3_PREFIX='your-project-prefix'
export AWS_REGION='us-east-1'
# Supply AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY through your environment.
: "${AWS_ACCESS_KEY_ID:?Set AWS_ACCESS_KEY_ID}"
: "${AWS_SECRET_ACCESS_KEY:?Set AWS_SECRET_ACCESS_KEY}"
```

Native CLI configurations must use `storage.engine: multiscale`; the legacy
engine remains local. For an S3 example, repeat the graph commands above with
`BWGRAPH_STORAGE_BACKEND=s3` instead of `local`, using a fresh `run_dir`.
Keep bucket, prefix and graph paths consistent when reopening a database.
Optionally set `BWGRAPH_S3_LOCAL_ROOT` to a common parent of graph directories to
make object keys relative to that root across machines.

Validate S3 with an isolated prefix; these tests retain remote objects:

```bash
export S3_TEST_PREFIX="validation/$(date +%Y%m%d-%H%M%S)"
bash tools/test_s3.sh build-release output/s3-release
bash tools/test_s3.sh build-txn output/s3-txn
```

For MonacGraph, use its cloud configuration and launcher:

```bash
# Run from the MonacGraph repository; keep the S3 environment and JAVA_HOME set.
export BWGRAPH_SOURCE=/absolute/path/to/Bw-Cloud-Graph
BWGRAPH_STORAGE_BACKEND=local bash scripts/bwgraph.sh build
bash scripts/bwcloud.sh test
bash scripts/bwcloud.sh start
```

Configure graph roots in `conf/bw-cloud.properties`; the launcher uses
`gremlin-server-bw-cloud.yaml`. See MonacGraph's README for Maven and Web UI setup.
The JNI service uses local RocksDB working metadata and checkpoints it to S3 on
normal close. Shut down cleanly before reopening; automatic crash recovery and
concurrent ownership by multiple processes are not supported.
