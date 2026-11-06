# Bw-Graph Project Guide and Working Agreements

This file applies to the entire repository and is intended for developers and AI coding assistants. Commands use `~/CProjects` as the recommended workspace; replace it consistently if needed. Do not hardcode the original machine's username in code. Keep any additional directory-specific instructions consistent with this guide and explain local differences.

## 1. Project Scope and Working Principles

Bw-Graph is a C++20 graph storage research project combining a Topology-Aware Tree with paged CSR for graph storage, dynamic updates, and graph analytics. See `README.md` for the published paper and citation. Future journal extensions must preserve a reproducible baseline and distinguish existing capabilities, experimental implementations, and unverified research hypotheses.

Before making changes:

1. Run `git status --short` and preserve existing user changes and untracked files.
2. Read the relevant headers, implementations, tests, and YAML files. Treat the code and build scripts as the source of truth for actual behavior.
3. Run the smallest relevant tests first, then expand validation according to the change. Do not silence warnings by globally disabling diagnostics or removing valid assertions.
4. Dependency policy: **install folly through Homebrew; build RocksDB and oneTBB from Git sources; use the specified preserved KaMinPar source tree.** Keep Git dependencies under `CProjects`, alongside Bw-Graph. Build each dependency in its own `build/` directory and install globally into `/usr/local`.
   Exception: research components ART and Louvain live in `third_party/` as pinned Git submodules and are built through project-local CMake targets; do not install them globally. Initialize them with `git submodule update --init --recursive`. Louvain supplies an OpenMP header-only target, a compatibility test, and the whole-graph community layout backend. It is not capacity-constrained; edge weights do not enforce community capacity limits.
5. Do not commit build directories, large raw datasets, database files, or machine-specific paths. Do not delete existing experimental data to resolve build problems.

## 2. Verified Baseline and Dependencies

Verified on 2026-09-23 on an Apple Silicon Mac with Apple Clang 21 and CMake 4.4.3: clean Debug and Release builds completed without warnings or errors, with 33/33 CTest entries passing in each configuration. Creating a fresh example database and running 10 BFS rounds also succeeded. Transactions, neighbor compression, and io_uring were disabled by default. These results do not establish that all optional configurations, Linux builds, or large-scale experiments have been validated.

| Component | Source and baseline | Purpose |
| --- | --- | --- |
| C++20 compiler, CMake, Make, Git, pkg-config | macOS CLT + Homebrew; KaMinPar requires CMake >= 3.21 | Build tools |
| folly | Homebrew; verified version 2026.09.21.00 | Concurrent containers, memory reclamation, and related utilities |
| RocksDB | Git v10.4.2 | Giant-vertex and property storage |
| oneTBB | Git v2022.2.0 | Parallel execution and allocators; matches the version specified by KaMinPar |
| KaMinPar | Preserved source HEAD `7f898e1ee3c6d2932df292a6b6f2afd11cf51173` | Graph partitioning |
| kassert | Git `988b7d54b79ae6634f2fcc53a0314fb1cf2c6a23` | KaMinPar build dependency; no separate build of its tests is needed |
| indicators | Git v2.3 | Progress output |
| gflags, glog, yaml-cpp, nlohmann-json, googletest | Homebrew | Flags, logging, configuration, JSON, and unit tests |
| libomp, zlib, snappy, lz4, zstd, bzip2 | Homebrew or system SDK | OpenMP and compression; macOS can use SDK zlib/bzip2 |
| google-sparsehash, hwloc | Homebrew | KaMinPar hash containers and the runtime dependency of oneTBB's tbbbind |
| liburing | Optional on Linux | Required when io_uring is explicitly enabled |

Homebrew does not pin all transitive dependency versions here. A fresh machine may receive different versions; record `brew list --versions` and validate again. Do not mix Homebrew TBB with the source-built TBB. After changing TBB, rebuild in this order: TBB → KaMinPar → Bw-Graph.

### 2.1 What Was Actually Done During the Migration

The September 23 session started with Command Line Tools, Homebrew, Git, the Bw-Graph checkout, and the preserved KaMinPar sources already present. It was not a test on a completely erased Mac. Section 3 adds bootstrap instructions for a new machine and presents the final installation workflow rather than repeating the intermediate attempts.

- Homebrew supplied folly and the other packaged dependencies. The correct sparsehash formula is `google-sparsehash`, not `sparsehash`. Ninja was installed during an initial attempt but is not required by the final Make-based workflow. Snappy, LZ4, and Zstd arrived as folly dependencies; Section 3 lists them explicitly for clarity.
- RocksDB was initially built under a temporary project-local `.deps` directory. After the directory and installation requirements were clarified, its sources and indicators were moved alongside Bw-Graph, RocksDB was rebuilt in its own `build/`, and `.deps` plus its temporary CMake changes were removed. Do not reproduce the abandoned local installation.
- TBB was initially installed through Homebrew. It was later replaced with Git oneTBB v2022.2.0, followed by a KaMinPar rebuild and reinstall. Removing brew TBB also removed hwloc automatically, so hwloc was restored for source-built tbbbind. A new machine should install source TBB directly and keep hwloc installed.
- KaMinPar used the preserved source tree and the sibling kassert checkout. A separate kassert configure attempt failed because its standalone tests expected an absent googletest submodule; it was unnecessary. The successful workflow uses kassert through KaMinPar's `FETCHCONTENT_SOURCE_DIR_KASSERT` without building kassert separately.
- The user executed the `sudo make ... install` commands in a local terminal because the assistant could not supply the sudo password. Successful compilation alone was not treated as successful global installation.
- After the final TBB switch, all registered CTest suites and an isolated BFS example passed. The latch test's unused atomic-load result was then explicitly discarded, and both exact clean-build script commands passed without warnings or errors. These historical results are from the September 23 session.

Documentation audit on 2026-10-04: dependency source revisions and surviving CMake caches agree with the final versions, options, Make generator, and `/usr/local` prefixes above. Temporary `/tmp` validation logs from September are no longer present; the earlier test results are session records, not newly rerun results. The current main-project cache is Debug and should not be described as a permanently retained Release build.

A separate, fresh CMake build directory was configured during this audit without explicit dependency `-D` paths. On this already provisioned Mac, configuration succeeded, OpenMP was found, and KaMinPar/RocksDB/TBB resolved under `/usr/local` while folly resolved under `/opt/homebrew/opt/folly`. This checks dependency rediscovery, not installation on an empty machine; no new full build or test run was performed for the documentation audit.

## 3. Setting Up a Fresh Environment

### 3.1 System Tools and Homebrew

On macOS:

```bash
xcode-select --install
```

After the tools are installed, install Homebrew using the [official instructions](https://brew.sh/) and apply the `brew shellenv` setup printed by the installer so that `brew` is available in new terminals. The current CMake configuration defaults to `/opt/homebrew` on macOS. Apple Silicon is the verified platform; Intel Macs require prefix detection adjustments and must not be described as already validated.

For a new Apple Silicon Mac, wait for Command Line Tools installation to finish, then run the official Homebrew installer if brew is not installed:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
eval "$(/opt/homebrew/bin/brew shellenv)"
xcode-select -p
clang++ --version
brew --prefix
```

The expected architecture is `arm64` (`uname -m`) and the brew prefix is `/opt/homebrew`. Follow the installer's instructions to persist `shellenv` in your shell profile. These are bootstrap instructions; installing CLT/Homebrew from scratch was not performed in the recorded migration. Run package and compiler commands as your normal user; use sudo only for steps that explicitly require it.

On Ubuntu, the complete server setup has not been verified during this migration. Install the system build tools first, then install and initialize Linuxbrew using the [Homebrew on Linux instructions](https://docs.brew.sh/Homebrew-on-Linux), preserving the requirement to install folly through brew.

```bash
sudo apt update
sudo apt install -y build-essential git curl ca-certificates pkg-config zlib1g-dev libbz2-dev
```

The compiler must support the C++20 features used by the project. System GCC and CMake versions on older Ubuntu releases may be insufficient; the presence of `g++` alone does not establish compatibility. Use a consistent compiler, standard library, and architecture for all source dependencies and the main project. Use fresh build directories when changing `CC` / `CXX`. ABI compatibility between Linuxbrew folly and the system standard library must be checked by building and running the project.

On both platforms:

```bash
brew install cmake pkg-config folly gflags glog yaml-cpp nlohmann-json googletest libomp google-sparsehash hwloc snappy lz4 zstd
export CMAKE_PREFIX_PATH="/usr/local:$(brew --prefix)${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:$(brew --prefix)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
mkdir -p ~/CProjects
```

These exports apply only to the current shell. Add them to your own shell configuration if persistence is needed. Do not substitute brew/apt RocksDB or TBB packages for the source builds below.

### 3.2 Obtaining the Project and Dependency Sources

Run these commands in a fresh workspace. Do not clone over or overwrite existing repositories.

```bash
cd ~/CProjects
git clone https://github.com/yaoyaowong/Bw-Graph.git Bw-Graph
git clone --depth 1 --branch v2022.2.0 https://github.com/uxlfoundation/oneTBB.git oneTBB
git clone --depth 1 --branch v10.4.2 https://github.com/facebook/rocksdb.git rocksdb
git clone --depth 1 --branch v2.3 https://github.com/p-ranav/indicators.git indicators
git clone https://github.com/kamping-site/kassert.git kassert
git -C kassert checkout 988b7d54b79ae6634f2fcc53a0314fb1cf2c6a23
```

The main repository also has a Gitee remote at `git@gitee.com:wangsy_thu/bw-graph.git`. Use a remote you can access and an explicit branch/commit. Do not assume that different remotes are automatically synchronized.

HTTPS cloning avoids assuming a fresh Mac already has an SSH key configured. If the repository is private, authenticate with an account that has access. Alternatively, configure GitHub SSH authentication before using `git@github.com:yaoyaowong/Bw-Graph.git`. The original migration used an existing checkout, so a fresh authenticated clone was not part of that validation.

**KaMinPar is deliberately excluded from the download commands above.** Copy or extract the preserved KaMinPar source tree into `~/CProjects/KaMinPar`, retaining local modifications and commit information. The current baseline uses this migrated source tree; do not replace it directly with the latest upstream version. If it is unavailable on a fresh machine, obtain a copy from the maintainer first. This is a prerequisite for reproducing the build. Check it with:

```bash
git -C ~/CProjects/KaMinPar rev-parse HEAD
git -C ~/CProjects/KaMinPar status --short
```

After copying sources, do not reuse the old machine's `build/`, `CMakeCache.txt`, or precompiled libraries. The following steps require fresh build directories. Back up existing caches or choose a different directory first.

### 3.3 Building and Installing oneTBB

```bash
cd ~/CProjects/oneTBB
cmake -S . -B build -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" -DTBB_TEST=OFF
make -C build -j 8
sudo make -C build install
```

### 3.4 Building and Installing RocksDB

```bash
cd ~/CProjects/rocksdb
cmake -S . -B build -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
  -DROCKSDB_BUILD_SHARED=OFF -DWITH_TESTS=OFF \
  -DWITH_BENCHMARK_TOOLS=OFF -DWITH_CORE_TOOLS=OFF \
  -DWITH_TOOLS=OFF -DWITH_TRACE_TOOLS=OFF \
  -DFAIL_ON_WARNINGS=OFF \
  -DWITH_SNAPPY=ON -DWITH_LZ4=ON -DWITH_ZSTD=ON \
  -DWITH_ZLIB=ON -DWITH_BZ2=ON
make -C build -j 8
sudo make -C build install
```

`FAIL_ON_WARNINGS=OFF` is part of the verified third-party RocksDB configuration. It only disables treating its warnings as errors; it is not a way to eliminate warnings in Bw-Graph. This builds the static library without requiring RocksDB's standalone tools or test programs.

### 3.5 Installing indicators

```bash
cd ~/CProjects/indicators
cmake -S . -B build -G "Unix Makefiles" \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DINDICATORS_BUILD_TESTS=OFF -DINDICATORS_SAMPLES=OFF
make -C build -j 8
sudo make -C build install
```

This is a header-only library, so an absence of compilation output from `make` can be normal. Installing the headers and CMake package is the important step.

### 3.6 Building and Installing the Preserved KaMinPar Source

```bash
cd ~/CProjects/KaMinPar
cmake -S . -B build -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DCMAKE_PREFIX_PATH="/usr/local;$(brew --prefix)" \
  -DTBB_DIR=/usr/local/lib/cmake/TBB \
  -DFETCHCONTENT_SOURCE_DIR_KASSERT="$HOME/CProjects/kassert" \
  -DKAMINPAR_64BIT_NODE_IDS=ON -DKAMINPAR_64BIT_EDGE_IDS=ON \
  -DKAMINPAR_64BIT_WEIGHTS=ON -DKAMINPAR_ENABLE_TIMERS=OFF \
  -DKAMINPAR_BUILD_APPS=OFF -DBUILD_TESTING=OFF
make -C build -j 8
sudo make -C build install
```

The 64-bit settings must be enabled when building KaMinPar itself and propagated to Bw-Graph through its exported target. Setting similarly named CMake variables only in Bw-Graph cannot change the ABI of an installed library. The actual weight option is `KAMINPAR_64BIT_WEIGHTS`; do not use the spelling `KAMINPAR_64BIT_WEIGHT_IDS` found in the main project to build the dependency. Disabling timers follows KaMinPar's guidance for calling its library interface from multiple threads.

On Linux, run `sudo ldconfig` after installing shared libraries. If package configurations are installed under `lib64/cmake` rather than `lib/cmake`, adjust `TBB_DIR` / `KaMinPar_DIR` to match. Execute the sudo commands individually or join them with `&&`; do not concatenate multiple commands without separators.

### 3.7 Initial Project Configuration and Validation

```bash
cd ~/CProjects/Bw-Graph
cmake -S . -B build -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/usr/local;$(brew --prefix)" \
  -DKAMINPAR_LOCAL_PATH=/usr/local \
  -DTBB_DIR=/usr/local/lib/cmake/TBB
cmake --build build -j 8
ctest --test-dir build --output-on-failure --timeout 120
```

To inspect dependency discovery and linkage on macOS:

```bash
rg 'KaMinPar_DIR|RocksDB_DIR|folly_DIR|TBB_DIR|OpenMP_CXX' build/CMakeCache.txt
otool -L build/bin/bfs
```

Use `grep -E` if `rg` is unavailable. RocksDB, KaMinPar, and TBB should come from `/usr/local`; folly should come from brew. For TBB libraries named with `@rpath`, inspect the link command or run with `DYLD_PRINT_LIBRARIES=1` to confirm the resolved paths. On Linux, use `ldd build/bin/bfs`.

## 4. Everyday Build Commands

**Run every `tools/build.sh` command from the repository root.** The script defaults to `build` relative to the current directory and uses Make.
Use `--build-dir DIR` for separate Linux, transaction, or other configurations;
keep copied build trees intact. Nonempty clean targets require a CMake cache for
this checkout, and foreign-source caches are rejected. Do not mix a Ninja generator into the same directory.

```bash
bash ./tools/build.sh -t release -c
bash ./tools/build.sh -t debug -c
```

With `-c`, each command removes the selected build directory after checking its cache,
configures CMake again, compiles all targets, and runs CTest. `-c` does not remove `workspace/`, but it deletes any temporary data stored inside `build/`. Do not run these two commands concurrently.

| Option | Actual behavior |
| --- | --- |
| No arguments | Debug, incremental build, tests enabled |
| `-t` / `--type` | `debug`, `release`, `relwithdebinfo`, or `minsizerel` |
| `-c` / `--clean` | Remove and recreate the selected build tree, after checking cache ownership |
| `--build-dir DIR` | Select a separate build directory (default `build`) |
| `-j N` / `--jobs N` | Build parallelism; defaults to `sysctl -n hw.ncpu` on macOS or `nproc` on Linux, falling back to 4 when unavailable |
| `--no-tests` | Skip running CTest, but still compile test targets |
| `--compress` | Enable `BWGRAPH_NEIGHBOR_COMPRESS` for PTV neighbor compression |
| `--txn` | Enable `BW_GRAPH_ENABLE_TRANSACTION` for MVCC |
| `--uring` | Enable `BW_GRAPH_ENABLE_IO_URING`; Linux only, requires liburing; macOS warns and disables it |
| `--jni` | Enable `BW_GRAPH_BUILD_JNI`; requires Java 17+; adds JNI library, JAR and JVM test |
| `-h` / `--help` | Print script help |

```bash
bash ./tools/build.sh -t release -j 8
bash ./tools/build.sh -t debug --no-tests -j 4
bash ./tools/build.sh -t relwithdebinfo -c
bash ./tools/build.sh -t release --compress -c
bash ./tools/build.sh -t debug --txn -c
# Linux only; install liburing-dev first:
# bash ./tools/build.sh -t release --uring -c
```

Feature flags start disabled on every script invocation. Pass the flags again to retain transactions or compression. Every invocation reconfigures, including after adding source files or changing CMake files.

`-c` discards custom dependency paths previously supplied with `-D`. Rediscovery of the standard global macOS installation has been verified. For custom installation prefixes, export `CMAKE_PREFIX_PATH` or use the manual CMake commands above. Do not solve package discovery by hardcoding another machine's paths.

After a clean build, CMake may print the default KaMinPar hint as `$HOME/Library/CppLib/KaMinPar`. That message alone does not identify the library actually selected: `find_package` can still resolve the installed `/usr/local/lib/cmake/KaMinPar` package. Check `KaMinPar_DIR` and the link command. On a fresh machine, finish all dependency installations before running the build script; no manual edit to `CMakeLists.txt` is required for the documented Apple Silicon layout.

## 5. Tests and Example Runs

```bash
cmake --build build --target test_latch -j 8
ctest --test-dir build -R '^test_latch$' --output-on-failure
ctest --test-dir build --output-on-failure --timeout 120
bash ./tools/run_tests.sh read_neighbor_clone
bash ./tools/run_tests.sh txn_snapshot --txn
```

`test/` uses GTest and CTest. There are currently 56 CTest registrations on macOS and 57 on Linux: 48/49 test
executables plus eight additional ART-backend invocations. Optional `--jni` adds
one real JVM test (57 on macOS, 58 on Linux). These include the separated
disk-format suite and Linux-only syscall fault suite; registrations are not GTest case counts. The thirteen programs under `bw-test/` are standalone integration/concurrency tests and disk/buffer benchmarks; they are not automatically run by CTest. `run_tests.sh` reconfigures the shared `build/` directory to Debug by default or Release with `--release`, disables compression, and sets transactions according to `--txn`. Use `--release` for performance measurements.

On a fresh machine, start with the included `data/example.graph`, not the full large-graph experiment scripts. Database construction reads `data/<graph.name>.graph`, so run programs from the repository root as well. Use isolated storage for initial validation:

```bash
mkdir -p build
sed -e 's|./workspace/example/|./build/smoke-example/|g' \
    -e 's|rebuild: false|rebuild: true|' \
    -e 's|redo: false|redo: true|' \
    config/example.yaml > build/smoke-example.yaml
./build/bin/bfs ./build/smoke-example.yaml --algo map --benchmark
```

This explicitly rebuilds the test database under a separate prefix, avoiding reuse of historical experiment data. You can also run `bash ./tools/example.sh` for BFS, WCC, and PageRank, but it uses the storage paths in `config/example.yaml` and creates or updates the corresponding databases.

## 6. Repository Structure and Reading Order

| Path | Contents and modification entry points |
| --- | --- |
| `CMakeLists.txt` | C++20 settings, dependency discovery, feature flags, the `bwgraph` library, and executable targets |
| `include/bw_graph/common/` | Basic types, configuration, locks, parallel utilities, logging, bitmaps, and the PTV codec |
| `include/bw_graph/storage/` | Page types, disk management, and I/O interfaces |
| `include/bw_graph/io/` | CSR/delta/index encoding and I/O, plus page builders |
| `include/bw_graph/buf/` | Buffer pools, page mappings, background CSR/delta flushing, and workload sketches |
| `include/bw_graph/index/` | Vertex indexes and version management |
| `include/bw_graph/part/` | KaMinPar partitioning, reordering, page deltas, and structural modification operations (SMO) |
| `include/bw_graph/mem/` | In-memory graphs and edge-list, CSR, and map representations |
| `include/bw_graph/algo/` | Vertex/edge map and edge scan operators |
| `include/bw_graph/db/` | Database entry points, transactions, external storage, and property storage interfaces |
| `src/` | Module implementations; `db_build.cc` constructs databases, `db_dyn.cc` handles dynamic operations, `db_algorithm.cc` implements graph algorithms, `txn.cc` handles transactions, and `smo.cc` implements structural changes |
| `bin/` | One CLI executable per `.cc`: bfs/wcc/pagerank/cdlp, import/partition, storage/throughput, dynamic_algorithms/htap/transaction, and others |
| `test/` | GTest suites organized by common/storage/io/mem/part/buf/db/algo/third modules |
| `bw-test/` | read_neighbor_clone, adaptive_consolidation, page_delta_concurrency, vertex_version_concurrency, txn_snapshot, multiscale_disk_bench, multiscale_buffer_bench, multiscale_loading_bench, multiscale_graph_bench, multiscale_smo, multiscale_txn, and multiscale_smo_bench |
| `config/` | Example/default and dataset-specific YAML files |
| `tools/` | Build, test, example, and batch experiment scripts |
| `data/` | Input graphs; large files are generally ignored, while example.graph is included in the repository |
| `workspace/` | Persisted graph data, indexes, and RocksDB databases; generally untracked |
| `logs/`, `output/` | Logs and experimental output; generally untracked |
| `build/` | Current generated files: `bin/`, `test/`, `bw-test/`, compilation database, and related artifacts |
| Old build directories such as `build-txn/` | Potential migration leftovers; not source code or portable build artifacts |
| `README.md`, `DEPENDENCY.md`, `AGENTS.md` | Overview/citation, dependency entry point, and this shared development/setup guide, including the migration record |

Suggested reading order: `config/example.yaml` → `common/config.h` / `common/type.h` → `db/db.h` → `src/db_build.cc` → `src/db.cc` / `src/db_dyn.cc` → relevant page/buffer/index/SMO modules → corresponding tests and CLI entry points.

The main data path is input graph → in-memory representation → partitioning/reordering → CSR pages and vertex indexes. At runtime, database interfaces coordinate buffer pools, delta updates, and SMO. The legacy path uses RocksDB for giants and properties; native multiscale mode uses it only for properties. Graph algorithms access the graph through database neighbor reads and map/scan operators.

Key configuration sections: `multiscale_buffer` controls six independent experimental frame budgets; `storage` controls CSR/delta/meta paths and page parameters; `buffer` / `delta_buffer` control caching and flushing; `partition` controls partitioning; `thread` controls consolidation and version GC; `algorithms` controls algorithms; and `graph.name` selects the dataset. `rebuild` / `redo` affect database reconstruction and partitioning. Use separate directories when changing disk formats, compression, or related layouts; do not assume existing files remain compatible.

## 7. Change and Regression Requirements

- Follow the existing C++20 style and `.clang-format` settings: 2-space indentation and a 100-column limit. Avoid formatting the entire repository for a small change.
- Place new implementations in the appropriate module rather than putting storage logic in benchmarks. Re-run CMake after adding `.cc` files. The build currently uses globbing for `src/`, `bin/`, `bw-test/`, and `test/`. Name tests `test_*.cc` and avoid duplicate target names across directories.
- Update configuration parsing, defaults, example YAML, and configuration tests together. Document compile-time flags separately from runtime settings.
- For page layout or serialization changes, check both readers and writers, page sizes, offset bounds, index persistence, and backward compatibility. Tests must cover writing data and reopening it for reading.
- For concurrency changes, examine lock ordering, atomic operations, version switching, hazard-pointer reclamation, and object lifetimes. Spans returned by `read_neighbor()` depend on the page and its lock lifetime. Release locks according to the interface contract and do not retain dangling views afterward.
- For flushing and SMO changes, check background thread shutdown, dirty-page persistence, disabled-worker settings, and read/write visibility. Transaction changes additionally require validation with `--txn`, snapshots, and concurrent version tests.
- For substantive C++ changes, run relevant tests first. Before delivery, verify that clean Debug/Release builds introduce no warnings or errors and that CTest passes. Validate affected optional configurations as well. For documentation-only changes, check paths, options, code blocks, and diffs; rebuilding all dependencies is unnecessary.
- Report what changed, the validation commands, pass/fail results, and unverified scope. Compilation success does not establish algorithmic correctness or performance improvement. Do not change test expectations to hide regressions.

## 8. Journal Extensions and Experimental Reproducibility

The following are agreements for future work, not claims of existing results or promised research conclusions:

1. Record the original paper's baseline code commit, dependency commits/versions, compiler, build type, feature flags, and complete YAML configuration. Use separate branches and database prefixes for extensions.
2. For each experiment, record CPU/memory/OS, SSD/filesystem, thread count, cache budget, flushing/SMO/GC settings, dataset source and checksums, preprocessing, directed/undirected and deduplication conventions, random seeds, commands, and raw logs.
3. Distinguish initial construction, database reuse, cold caches, warmup, and steady-state measurements. Measure performance in Release mode and state whether partitioning, import, warmup, and flushing time are included.
4. Preserve the baseline and design ablations around explicit hypotheses, such as delta/SMO policies, compression, concurrency control, or I/O backends. Change only a small number of factors at a time. Do not attribute differences across hardware or cache budgets directly to algorithmic improvements.
5. Repeat runs, report sample counts and variability, and retain reasons for failed or anomalous runs. Do not fabricate, cherry-pick, or manually substitute experimental data. `example.graph` is only a smoke test, not evidence for paper performance claims.
6. Batch scripts may hardcode datasets and output paths. Inspect `tools/run_*.sh` and check data availability before executing them. Experimental data is generally gitignored and needs separate archival. Confirm that new analysis scripts are actually tracked by version control.
7. As research designs, figures, and analyses grow, place them in separate `docs/` or experiment documents, creating those directories as needed. Keep this file focused on stable engineering rules and entry points. Update documentation when capabilities change, and do not describe planned features as implemented.

### Ubuntu storage-only validation (2026-11-01)

- Ubuntu 20.04 / GCC 13 / ext4: independent raw disk-manager Debug and Release
  builds passed 2/2 CTest executables (20 GTest cases each). The raw disk benchmark
  passed all sampled rounds. The same 20 cases passed ASan/UBSan and leak checking
  outside the ptrace sandbox (the sandbox LeakSanitizer failure is retained in logs).
  This is a component result, not full-system validation.
- Use `cmake -S cmake/storage-validation -B build-linux-storage-check`, then build
  and run `cmake -E chdir build-linux-storage-check ctest --output-on-failure`.
  CTest 3.16 ignores `--test-dir`; do not count “No tests were found” as a pass.
  `TMPDIR` selects the test filesystem. Full-format persistence checks remain in
  `test_multiscale_disk_formats`, requiring the full dependency environment.
- Linux fault injection checks aligned short pread/pwrite, EINTR, partial-write
  and fdatasync failures retaining dirty epochs, and async reads directly into
  caller memory with shutdown draining. No production I/O backend was replaced.
- Complete Ubuntu graph validation remains blocked: no Linuxbrew folly; installed
  RocksDB 10.8.0 and TBB 2022.3.0 differ from required 10.4.2 and 2022.2.0;
  located KaMinPar sources do not match the preserved revision; sudo requires
  interactive authentication. Do not substitute those installed libraries and
  describe the result as reproducing the dependency baseline.
- RN/YT inputs were present and hashed; graph runs, Buffer Pool integration,
  SMO/TAT, transactions, layout comparisons, equal-thread algorithm comparisons,
  cache-budget sweeps and memory accounting remain unverified. Preserve raw logs
  under an independent `output/` directory. Do not infer graph performance from
  the raw disk benchmark or present its unequal-thread paths as a speedup study.

### 8.1 Multiscale storage prototype (2026-10-29)

- `storage/page_id.h` defines a strong 32-bit ID: 3-bit space plus 29-bit slot.
  Six selectors cover CSR 4/32/256 KiB/2 MiB (each step is 8×), Index, and Delta; selectors 6–7 are
  reserved. Do not pass encoded IDs into legacy disk-manager APIs as local page numbers.
- `io/multiscale_csr_page.h` defines the independent, uncompressed CSR image;
  counts/offsets/vertex slots are 32-bit. Its serialization remains separate from
  the legacy CSR format, and deserialization currently copies the image.
- `storage/multiscale_disk_manager.h` defines one manager directly owning six data
  fds, plus a move-only aligned `disk_page_t` and per-page future/request handles.
  It does not compose six legacy managers. Each file has a 4 KiB versioned header;
  store version 2 rejects the earlier prototype headers. Offsets are
  `4096 + slot * page_size`. Allocation is append-only, recovered from
  complete file length including all-zero slots. There is no page-reuse API yet.
- Async single/batch reads use a bounded queue and fixed workers. Submission can
  block under backpressure; result memory is caller-owned and not bounded by queue
  capacity. This is API-level batching, not native kernel batch submission. The
  new manager does not use the legacy optional io_uring backend. Shutdown drains
  accepted requests before syncing/closing; futures may outlive the manager.
- macOS uses F_NOCACHE/F_FULLFSYNC; Linux has O_DIRECT/aligned-buffer/fdatasync paths,
  with explicit errors instead of silent buffered fallback. The raw disk-manager Linux branch was subsequently checked on Ubuntu (see above);
  this does not establish Linux validation of the Buffer Pool or graph engine.
- The default legacy DB remains unchanged; opt-in graph integration is described in §8.2.
  Native extent SMO is described in §8.2; Native TAT maintenance is described in §8.2; ACGraph remains unintegrated.
  Use separate store directories; old Index files have a different container format.
  No crash-atomic page updates, directory-entry durability, or recovery protocol.
  Caller synchronization is required for same-page read/write and flush boundaries.
- Tests are discovered normally in `test/io/` and `test/storage/`. Benchmark:
  `bash tools/run_tests.sh multiscale_disk_bench --release --pages 65536 --rounds 3`.
  Its MTR-style console output is concise. See README for timing/cache semantics,
  commands and limitations. This is a basic performance check: report results in
  the console/chat; do not add benchmark samples under `docs/` or results to README.
  Do not compare buffered and bypass-cache numbers as if equivalent.
- `buf/multiscale_buf_pool.h` adds an independent six-group pool with aligned chunks,
  separate frame controls, RAII pin/read/write guards, CLOCK eviction, coalesced loads,
  synchronous inline misses and direct-to-frame asynchronous reads. Hits use a Folly
  concurrent map and CAS pins, then verify identity to reject stale map entries. Eviction
  CAS-reserves zero-pin frames before reuse; READY publishes bytes with release ordering.
  Pins own chunks and use the project's compact content latch; per-frame hit counters and
  interleaved chunk allocation avoid global hot-path counters. No group latch is taken on
  hits. Unique synchronous misses allocate no future; coalesced waiters still use futures.
  Never hold slow-path group latches across I/O or content locks. Dirty publication precedes
  release unpin. Dirty writeback failures preserve the victim; failed reads free reservations.
- Disk flush tracks per-file write epochs, published after allocation/write attempts even
  on transfer failure. Write epochs advance atomically after transfer attempts; per-file
  sync mutexes serialize sync operations, and only successfully synced epochs become clean. The first flush covers every opened file. Subsequent flushes
  skip unchanged files. Do not replace this with a dirty bool that can lose concurrent writes.
- Configure `multiscale_buffer.{csr_4k,csr_32k,csr_256k,csr_2m,index,delta}` with
  `chunk_count` and `chunk_size` (pages/chunk). Pass `BW_MULTISCALE_BUFFER_CONFIG`
  explicitly to the new pool. Zero chunks disables a space; legacy budgets are separate.
  Configuration parsing/defaults/examples and `test_multiscale_buffer_config` must agree.
- Async results own pins, not content locks. Consume/discard results to release capacity;
  acquire/destroy content guards on the same thread. Full pinned groups report an error.
  Submission can block for queue space or dirty-victim writeback. Release guards before
  flushing. Stop producers and release pins before explicit close; false means retry after
  releasing pins. Destructor-only shutdown cannot guarantee persistence with outstanding
  pins or I/O failures. Disk manager lifetime must cover the pool and pending requests.
- `io/multiscale_csr_page_view.h` borrows frame bytes: O(1) header validation, per-vertex
  bounds checks, no whole-page copy/decode. Keep the guard alive. Full integrity validation
  still uses owning deserialization; legacy Index/Delta typed readers are not adapted.
- Buffer tests live in `test/buf/`; integrated benchmark:
  `bash tools/run_tests.sh multiscale_buffer_bench --release --pages 8192 --rounds 3`.
  It compares aligned disk reads with cold buffer reads, hot reads/writes and dirty eviction.
  See README for queue-depth, logical-byte and flush timing semantics. No background
  flusher or workload-driven resizing is provided; graph-path integration is opt-in (§8.2).
- Previous disk-only macOS validation (2026-10-28): clean Debug/Release builds, 37/37 CTest entries in each, and
  ASan/UBSan on the new disk test passed with transactions/compression/io_uring off.
  The build script and Release standalone-test runner were also exercised.

- Buffer extension validation (2026-10-29, macOS): clean Debug/Release builds without
  compiler warnings, 39/39 CTest entries in each, ASan/UBSan and ThreadSanitizer on
  26 buffer cases passed. Transactions/compression/io_uring were off. The Release
  standalone-test runner was exercised; Linux remains unverified.

- `bw-test/multiscale_loading_bench.cc` compares legacy/new caches on the same physical
  4 KiB file with equal frame/chunk budgets, and separately models fixed 16 KiB vs adaptive
  CSR adjacency loading with equal 32 MiB cache budgets. The latter packs small vertices
  and splits oversized fixed-page adjacency lists; it does not benchmark RocksDB or full
  graph execution. Both disk-only/cache batch paths deduplicate within a batch. Prefetch
  assumes a known frontier. See README for cache policy, work, padding and timing semantics.
  Run `bash tools/run_tests.sh multiscale_loading_bench --release --suite all`.
  Keep raw results outside tracked documentation; report regressions as well as improvements.

### 8.2 Native graph integration and SMO (2026-10-31)

- `storage.engine: multiscale` selects the native six-fd path. Workers >0 enable native
  extent consolidation; zero keeps base immutable. Compression is rejected. Fresh store
  prefixes are required. Index format 7 is incompatible with previous prototypes, and
  transaction/non-transaction stores cannot be interchanged.
- Every vertex has a 4 KiB CSR anchor slot. Small adjacency starts inline; giants use
  private multiscale extents. Current base locations/lengths are authoritative in resident
  Index generations. Anchor IDs remain stable as payload moves; current leaf IDs track CSR slot pages.
  Native TAT maintenance is implemented; configurable Array/ART runtime indexes are
  described in the MVVI integration section below.
- Delta ownership is per anchor, with persistent page links and per-vertex record links.
  An update publishes a record only after releasing its page write guard. Native writers
  use anchor mutexes and non-owning, identity-checked write-frame hints; the background
  queue uses the configured worker count. Repeated updates coalesce Index dirty publication;
  checkpoint must remain quiescent. Queued failures
  must reach callers through `wait_for_smo`, checkpoint and close.
- SMO builds immutable replacement generations and publishes shared ownership. Borrowed
  ranges retain their generation; old disk slots remain allocated. `read_neighbor` is
  base-only. `read_neighbor_with_delta` includes an exclusive `delta_stop`; callers must
  stop there instead of replaying merged history. Map/scan still consumes base only.
- Workers batch up to 32 ready anchors, but rewrite each current leaf independently.
  Include unchanged slots when consolidating a leaf. Overflow uses Gorder + sequential
  byte-capacity packing, never ID-midpoint splitting. No-split rewrites preserve slot order.
  Giant metadata gets a new 4 KiB leaf slot; its stable anchor still owns its Delta chain.
  Giant changes rewrite affected page ranges, and unchanged
  extents remain in place. Hot ranges can split into smaller pages; read-dominant fragmented
  bases can coalesce. Hotness counts repeated updates, including repeated writes to one edge.
  The initial byte-equivalent cost policy is an explicit heuristic,
  not a calibrated optimum. SMO currently materializes the giant to discover boundaries.
- `consolidate_all_pages()` requires quiescent producers and forces residual work when SMO
  is enabled (subject to snapshot watermark). Its quiescent drain orders pending anchors by
  original page ID; online scheduling remains FIFO. Close/checkpoint persist remaining Delta;
  they do not force all deferred updates into base. Warmup/close also require quiescent
  readers. Mutable warmup does not retain obsolete frame pins and visits only live pages.
- `index/multiscale_tat.h` maintains a balanced resident directory with physical CSR leaf
  IDs and logical internal IDs. Whole-leaf replacements propagate overflow to the root.
  Internal splits reconstruct a base-generation quotient graph, run Gorder, and pack at
  60% of the 256-child limit. This replaces the legacy dense bitmap representation; do
  not describe its logical internal IDs as independently allocated disk-page IDs. Index
  format 7 checkpoints the directory/membership alongside vertex metadata, through the
  multiscale disk manager. Old immutable adjacency versions do not depend on current TAT
  membership. Quiescent drain/warmup refreshes algorithm scan order from the current tree.
  Ordinary edge reads/writes take no TAT mutex. No leaf-underflow merging/page reuse yet.
- Native edge transactions provide snapshot registration, timestamp-filtered Delta reads,
  atomic visibility across a committed write set, same-edge write conflict detection and
  read-your-writes. CSR generations/pages store a visibility floor; operations newer than
  the earliest active snapshot stay in Delta. Native transactional vertex lifetime changes
  and serializable mode are rejected. Direct vertex changes require no active snapshots.
  Do not claim the legacy transaction implementation was redesigned by this integration.
- `multiscale_buf_pool_t::new_page` is for freshly allocated, unpublished IDs only. It
  initializes a zeroed frame without reading its disk slot; existing fetch APIs preserve
  existing bytes. Dirty-victim failures still preserve the victim. Runtime allocation
  reserves <=256 KiB per space (one page for larger scales) to amortize file-length updates.
- Allocation remains append-only: no disk-page reuse/reclamation, retained-Delta repacking,
  crash-atomic checkpoint or WAL recovery. Index images/controls/versions are additional
  to the six frame budgets. SMO has <=32 KiB Delta scratch per worker plus temporary merge
  vectors. Keep these costs explicit when reporting reduced Buffer Pool budgets.
- `bw-test/multiscale_smo.cc`, `multiscale_txn.cc` and `multiscale_smo_bench.cc` cover writes,
  concurrent SMO, snapshot API and dataset comparison. Benchmarks use fresh stores, count
  foreground append as the primary rate and append plus drain plus close as a secondary rate,
  verify every reopened adjacency, and report automatic
  jobs separately. The harness defaults to KaMinPar + Gorder with TAT enabled, logs the
  layout and validates tree ownership/parents before timing and after reopen. ID order is
  an explicit `--layout id-based` ablation; `--layout louvain-gorder` selects communities. Never report a failed reference run as a speed comparison. Preserve
  dataset settings, writer/SMO thread counts and raw samples; do not put samples in README.
- Legacy reference fixes: frozen reservations for split sizing; actual anchor slots for
  ID-built giants; waiting for accepted arena submissions rather than only task-group
  membership; serialized forwarding lookup/installation and old-chain append; and actual residual work in `consolidate_all_pages`. Regression suites cover
  these paths. `tools/benchmark_smo.py` runs sequential comparisons/sweeps with raw logs and input hashes.
  Topology-path fixes additionally cover compact CSR/Index metadata offsets, persisted
  Index counts and reparse, unique leaf ownership, isolated subtrees, coarsened-build SMO
  wiring and parent persistence, and repeated-SMO root/parent maintenance. Label the
  reference as correctness-fixed, not byte-identical historical code.
- The full Linux graph engine remains unvalidated. The subsequent Ubuntu raw-disk
  checks and historical sanitizer/optional-feature results do not certify SMO code.

Topology-SMO validation on macOS (2026-10-31): clean Debug, Release and Debug transaction
builds passed 41/41 CTest executables without compiler warnings/errors. Native MTR also
passed with `--partition` (KaMinPar/Gorder), including two SMO workers, concurrent reads,
retained old views, giant shrink/reopen and transaction visibility. New regressions check
physical Gorder split order, recursive TAT splits/root growth, serialized membership,
compact CSR/Index headers and repeated legacy TAT SMO/reopen. Dataset comparison logs
must include `layout=kaminpar-gorder` and actual TAT counters; do not reuse ID-layout
speedup claims for this path. Performance gains are workload-dependent, not guaranteed.

SMO validation on macOS (2026-10-30): clean Debug and Release builds using `tools/build.sh`
passed 41/41 CTest executables without compiler warnings/errors. A clean Debug `--txn`
configuration passed 41/41 as well. Native regression tests covered concurrent
cross-anchor updates with two SMO workers, old borrowed views, giant shrink/reopen,
contiguous 2 MiB runs across SMO, stale write-frame hints after eviction,
cross-anchor commit visibility, write conflicts and advancing snapshot watermarks.
Legacy read/consolidation/concurrency MTR and `txn_snapshot` also passed. No Linux or new
sanitizer certification is implied. Fresh CSR output receives a CLOCK second chance only
after reuse; new Delta pages retain normal admission. The cache-sweep harness can rebalance
CSR/Delta fractions while preserving total frame bytes and writer/SMO counts. Its minimum
frame check describes the tested concurrent working set, not a universal memory lower bound.

Native-engine validation on macOS (2026-10-29): clean Debug/Release builds and subsequent
API regression checks passed all 40 CTest suites without compiler warnings; the Release
build script also passed. Legacy read/consolidation/concurrency MTR suites and native
read/concurrency MTR modes passed. Transaction-enabled CTest passed 40/40 and txn_snapshot
passed. Compression-enabled CTest passed 38/40: test_smo and test_builder failures were
reproduced from unchanged HEAD 88cf7ee in a separate source/build directory. Do not claim
that compression is fully validated or change expectations to conceal these failures.

Sanitizer scope: 28 buffer cases passed ASan/UBSan and TSan. The four native-store cases
passed ASan/UBSan after relinking the same instrumented objects without the unused RocksDB
archive; the normal full link reports a container-overflow in RocksDB static initialization
before main. Native-store TSan passed with TBB import scheduling limited to one thread,
while explicit concurrent reader/writer and vertex-append std::threads remained enabled.
Full parallel-import TSan reports in the prebuilt TBB scheduling path remain unresolved;
these targeted checks are not whole-DB sanitizer certification. No diagnostic suppression
was added to production code. Linux execution remains unverified.

## 9. Common Build and Migration Problems

- **CMake points to an old username:** use a fresh build directory instead of copied caches. Check `CMAKE_HOME_DIRECTORY`, the compiler, and dependency `_DIR` entries.
- **A dependency is missing:** confirm installation first, then inspect `CMAKE_PREFIX_PATH`, `TBB_DIR`, `KaMinPar_DIR`, `RocksDB_DIR`, and `folly_DIR`. Prefer exported CMake targets over manually linking `.a` files and losing transitive dependencies.
- **TBB versions are mixed:** inspect both KaMinPar and Bw-Graph caches and runtime library paths. After changing TBB, rebuild and reinstall KaMinPar, then rebuild Bw-Graph.
- **OpenMP is not found:** check libomp and CMake detection. Do not ignore the warning and claim the parallel experiment environment is ready.
- **sudo requires a password:** the user must complete installation in their local terminal; never send passwords through chat. Use sudo only for installation, not for compiling the main project.
- **Parallel builds run out of memory:** lower parallelism, for example with `-j 4`. Do not remove modules or skip failing targets to claim success.
- **macOS exception matching:** on the current dependency-linked benchmark, `std::overflow_error` was observed escaping a `std::exception` catch; private standard-library RTTI symbols are present in the executable. The new benchmark explicitly catches overflow/YAML exceptions and has a final catch-all so malformed input exits with failure instead of aborting. This is a local interoperability observation, not a global dependency repair or a Linux finding.
- **Network access is restricted:** resolve Git/brew/download connectivity first. SSH reverse forwarding only forwards ports; applications still need proxy settings. Keep personal proxy addresses out of the project and do not disable TLS certificate verification.

## 10. AGENTS.md vs. AI.md

This repository uses the standard filename `AGENTS.md`, not `AGETNS.md`. Codex discovers instruction files such as `AGENTS.md` by directory. `AI.md` is a custom filename and does not automatically become a Codex instruction file by default. Long-term research background notes can live in `AI.md` or `docs/`, with an explicit reference here. Do not maintain duplicate, conflicting build rules in two files.

See the [official documentation](https://learn.chatgpt.com/docs/agent-configuration/agents-md) for discovery order, `AGENTS.override.md`, and fallback filename settings. Verify automatic loading behavior separately for other AI tools; do not assume they all follow the same rules.

### 8.3 Explicit construction layouts (2026-10-31)

- `partition.layout` accepts `louvain-gorder`, `kaminpar-gorder`, or `id-based` and overrides
  old `storage.id_aware` / native partition routing flags. Without the key, retain legacy
  routing. Reopening does not change an existing store's physical layout; rebuild separately.
- `src/louvain.cc` runs the pinned OpenMP dependency on the complete undirected graph,
  including giants and isolates; uses all available OpenMP processors and restores team
  settings afterward. Internal keys are offset to avoid the dependency's local-moving
  zero sentinel; original public IDs are unchanged. Aggregated community zero is valid.
  Empty-edge graphs bypass modularity division and use singleton communities.
- Both topology layouts reorder each induced community/partition with Gorder, then pack
  continuously across community boundaries. One partial page per tiny community would
  inflate sparse-graph storage; do not restore that behavior. Shared boundary pages have
  exactly one TAT owner. Louvain does not reuse KaMinPar caches or impose a capacity bound.
- `test_graph_layout` covers YAML precedence/fallback, invalid names, zero/isolated vertices,
  singleton-page packing, all three native construction paths, writes/SMO and reopening.
  MTR `multiscale_smo` and `multiscale_txn` additionally accept `--layout louvain-gorder`.
- `tools/benchmark_smo.py --layouts ...` measures multiple layouts sequentially with fresh
  stores. Foreground throughput is primary; keep SMO enabled, and report the final drain
  and close separately. `--bfs-datasets rn yt` adds full-graph, warm BFS checks with identical
  seeded sources; `--bfs-only` skips write runs. Save actual Louvain thread/community counts,
  per-round samples and effective YAMLs. Parallel Louvain is not deterministic.

Layout validation on macOS (2026-10-31): clean Debug and Release builds via `tools/build.sh`,
and a clean Debug transaction build, each passed 42/42 CTest executables without compiler
warnings/errors. Louvain MTR passed concurrent SMO, retained views, giant shrink/reopen,
and transaction snapshot/conflict/atomic-commit checks. Fresh RN/YT comparisons use the
same final source for all three layouts; raw samples and source/configuration manifests
are retained outside this document. Linux remains unverified.

### 8.4 BYO unweighted algorithm port (2026-10-31)

- `algo/byo.h`, `algo/byo_graph.h` and `src/byo.cc` adapt the active symmetric/unweighted
  BYO suite at `00f8ead3925a3346d4322fd2b6f0b1ba1f1f9834`. Keep the MIT notice
  `algo/BYO_LICENSE.txt`. Do not describe this as the unchanged upstream binary.
- Reuse oneTBB, Folly F14 and CMake. Do not add BYO's container benchmarks, Bazel,
  Parlay scheduler, weighted adapters or graph-copy import path to this port.
- `neighbor_range_t::visit_chunks` lends page-local bytes to a synchronous callback;
  returning true stops the scan. Do not retain spans after the callback or send them to
  unjoined work. `base_degree` matches base-only visibility and never clones neighbors.
  The immutable path can read degree metadata directly; mutable SMO uses the published
  generation. Algorithm degree caches must be reconstructed after mutations/SMO.
- Whole-graph analytics require a stable, symmetric, deduplicated base. They are not
  transaction snapshots. PageRank is double-buffered with dangling redistribution;
  Brandes includes source dependency and handles unreachable vertices explicitly.
  Coloring/MIS require loop-free input. Keep K-core/triangle/spanner implementation
  adaptations and their scratch memory documented in README.
- `test_byo` checks all kernels against references/invariants, three layouts, isolates,
  zero-edge graphs, tiny caches, giant fragments, Delta exclusion and post-SMO generations.
  The multiscale graph test checks chunk scanning through all CSR scales.
- `byo_bench` / `tools/benchmark_byo.py` compare old/reference/byo using identical graph,
  source seeds, worker counts and warmup. Timers include algorithm allocations and results;
  adapter preparation is separate. Preserve baseline memory ordering and PageRank semantics.
  Never use the old BFS parent-chain-to-distance materialization as a performance baseline.
- Frame sweeps must retain all giants and enough demand frames for the requested workers.
  Report infeasible budgets and timeouts. Do not hide neighbor copies, degree arrays,
  algorithm scratch or extra threads outside a claimed total-memory limit. Current budgets
  measure data frames only. Linux and full-application sanitizer coverage require separate work.
- A one-frame-per-worker capacity check is a lower bound, not guaranteed admission under
  concurrent CLOCK/pin transitions. Preserve intermittent exhaustion results at that boundary;
  distinguish the lowest repeatedly passing configuration from a proven minimum.

BYO validation on macOS (2026-10-31): clean Debug and Release builds and a clean Debug
transaction build passed 43/43 CTest executables without compiler warnings/errors. Expanded
algorithm/storage checks were rerun in both Debug variants; the final Release clean build
includes them. The subsequent benchmark-only chunk geometry/cleanup changes also compiled
without warnings. Dataset comparisons independently check every BFS distance, component ID
and fixed-iteration PageRank value outside timing. Enabling transactions validates coexistence
with the transaction build, not transactional snapshots for these static analytics. Linux and
new sanitizer runs were not performed for this port.
The complete CLI suite (including all remote-read widths) also completed on RN and YT
with non-isolated BFS/Brandes sources. Baseline comparisons and descending frame sweeps
retain per-run results and boundary failures outside this document; passing small-budget
runs do not establish a total-process memory limit or constant performance under eviction.

### 8.5 ACGraph-inspired asynchronous analytics (2026-10-31)

- `algo/async/{algorithms,page_executor}.h`, `src/async_algo.cc` and
  `src/async_page_bfs.cc` implement optional native-base analytics. Local source model:
  ACGraph `d9aac49331a503664b3b7d9981ce9f15690e69ed`. This is an independent adaptation,
  not an untouched ACGraph binary or native io_uring port. No additional dependencies.
- Distinguish barrier-free BFS/CC page worklists (`async-*`), BYO kernels with async I/O
  (`hybrid-*`), and bounded page-local BFS (`page[-io]-bfs`). Fixed-iteration Jacobi PR
  must not be compared to ACGraph residual PR without matching accuracy/stopping semantics.
- Keep a single executor per active page, coalesce duplicate vertices, publish a BFS
  distance/parent pair atomically in the worklist variant, and drain accepted I/O on exceptions.
  No content guard crosses a thread boundary. `try_pin_page` is a resident-only lookup without I/O.
- Every successful page-BFS distance relaxation must publish a release notification;
  page processing consumes it with acquire. Do not suppress this notification because
  a smaller page priority already exists: concurrent wave startup can lose that wakeup.
- Page BFS coalesces page bits and bounds speculative distance propagation. The distance
  and parent outputs hold compact state, with one activation bit per vertex and page
  metadata. A short writer lock in the distance high bit serializes parent writes; do not
  publish a completed result with lock bits set. Compact mode requires fewer than 2^31
  vertices. Keep `page-packed` as the explicitly charged extra-label-array ablation.
  No adjacency shadow cache or independent low-degree adjacency array.
- All modes require a quiescent base, and degree/scan metadata must match that generation.
  CLI disables SMO; transaction-build success does not establish analytics snapshot isolation.
- CLI/benchmark keep active compute plus I/O within the requested allowance. With the
  current four-worker disk manager, pipelining at a 15-thread allowance uses 11 compute
  tasks; fully resident / low-headroom fallbacks use synchronous execution. The lower-level
  options API counts compute tasks explicitly. Do not hide extra threads in comparisons.
- Preserve frame-only and `--charge-auxiliary` sweeps. The latter reserves new scheduler
  and state memory before allocating frames; it is not total RSS. Common indexes, degree
  metadata, baseline scratch, outputs and validation memory remain separately reported.
  Never grow an auxiliary edge cache as the pool shrinks. Save regressions and failures.
- Keep existing BYO defaults as a reproducible control. No 2x speedup guarantee follows
  from adding asynchrony. Linux native submission and full-application sanitizers remain
  separate validation work.

Async analytics validation on macOS (2026-10-31): the final compact implementation passed
clean Debug, clean Release and clean Debug-transaction builds, each with 43/43 CTest
executables and no compiler warnings/errors. Tests include all BYO kernels through the
hybrid adapter, three layouts, parent/distance references, giant/long-path concurrent
reactivation, callback-exception draining and resident-only pin behavior. The compact
page and page-I/O variants passed 160 RN/YT distance-checked stress rounds before the
final budget sweeps. Hybrid CLI all-suite runs passed on both datasets. A development
notification-coalescing optimization lost a wakeup and failed a YT reference comparison;
its source/logs are preserved and the final implementation publishes every improvement.
Keep the failed development run out of performance conclusions. Fifteen-demand-frame
boundary runs intermittently exhaust the pool, even if another repeat passes. A YT
minimum-budget timing anomaly was repeated and both samples retained. No claim of a
universal minimum memory budget, universal speedup, Linux validation or new sanitizer
coverage follows from these checks. Raw data stays in ignored experiment output.

### 8.6 Configurable Array/ART MVVI (2026-11-04)

- `index.vertex_index: array|art` is a runtime YAML setting, defaulting to `array`
  on each configuration load. Instances capture the choice at construction; changing
  globals never changes an existing index. Both legacy and native paths use it.
- `index/vertex_table.h` provides stable controls: chunked Array or the pinned
  ART-OLC dependency with separately owned leaves. ART node structural mutation is
  excluded from lookups with a shared mutex to avoid upstream non-atomic node races.
  SMO changes the payload's version pointer, not the ART leaf. Existing hazard-pointer
  GC and native generation/snapshot rules remain authoritative.
- The legacy vector is also replaced by stable chunks in Array mode to avoid dangling
  references during growth. The `array-mvvi` tag at `b27b6e4` preserves the original
  implementation. No tag push or code commit is part of this integration.
- Disk formats remain unchanged; reopening Array-written data with ART and vice versa
  is supported. Public graph IDs remain dense, with retained controls for deleted IDs.
  Native transactional vertex lifetime changes and concurrent snapshot analytics remain
  unsupported; do not imply that choosing ART changes these API restrictions.
- Index accounting reports control/directory estimates, excluding ART internal nodes,
  epoch bookkeeping and allocator overhead; it is not total index memory or RSS.
- Legacy Delta chains own the allocation pin until `fetch_deltas_with_mark` copies
  their records. Never release that pin in `alloc_delta_page`: otherwise chain pointers
  can refer to recycled frames. The tiny-pool regression checks this ownership.
- MTR `force_consolidate` helpers explicitly request threshold 1; relying on a globally
  raised threshold can accidentally skip the version-reclamation path being tested.
- `tools/validate_vertex_index.sh BUILD_DIR LOG_DIR` runs legacy read/version/Delta
  concurrency plus native SMO under three layouts, for both backends. Transaction builds
  additionally run legacy snapshots and native snapshot/conflict/atomic-commit suites.
  Selected CTest executables have an additional `_art` invocation. Preserve failed
  development logs and report only the final verified configurations as passing.

- Transaction-enabled legacy direct edge writes use autocommit timestamps and the same
  commit publication lock as explicit transactions; direct CSR insertion is routed
  through Delta to retain snapshots. New snapshots acquire that publication lock.
  SMO may merge committed records at or below the oldest snapshot timestamp, consistent
  with the reader's inclusive visibility rule; uncommitted records remain deferred.
- Missing legacy TAT files rebuild from borrowed Buffer Pool frames. Release their
  latches rather than deleting them, mark modified parents dirty, and allocate logical
  Index IDs in the descending reserved high range.
- Legacy SMO reserves replacement pages against recursive consolidation while publishing
  per-vertex locations and TAT links. Releasing those claims early allows a second
  SMO to race the first publication. Concurrent test helpers protect a live vertex
  version until they own a frame pin; never cache an unprotected CSR Page ID across SMO.

Final Array/ART validation on macOS (2026-11-04): separate Debug, Release and Debug
transaction builds each passed 53/53 CTest registrations with no compiler warnings/errors.
The final MTR matrix passed 12/12 in Debug, 12/12 in Release and 20/20 with transactions,
covering both backends and all three native layouts. Release and transaction builds each
passed 126 example CLI build/reopen/algorithm invocations; algorithm reference checks
remain in CTest/MTR (CLI exit success alone is not an independent result oracle). The
final Release version also passed 20 repeated version-GC concurrency runs. Raw logs,
failed development runs, commands and source hashes are under `output/art-mvvi-20261104/`.
These checks do not establish an ART speedup, Linux validation, sanitizer coverage,
compression validation, or support for previously unsupported snapshot analytics.

### 8.7 Native empty-graph growth

- `bw_graph_db_t::create_empty(name)` creates native storage without an input graph;
  `open(name)` detects native stores, and `bulk_load(name)` preserves configured import.
  Source: `src/multiscale_cold.cc`; tests: `test/db/test_cold_start.cc`.
- `append_vertex()` allocates internal IDs. Compatibility explicit-ID insertion reserves
  skipped IDs as deleted slots; never treat them as live edge endpoints. Vertex-with-
  neighbors validates all neighbors before allocation and inserts directed outgoing arcs.
  Both file import and cold insertion maintain OUT(src) and IN(dst), not a second logical arc.
- Reception pages pack at most 126 empty vertex slots in ID order. Seal before edge
  updates/SMO; restore an untouched final page on reopen. Persist the initial empty
  TAT/Index even when no vertices have been added. Retain CSR-4K/Delta frame budgets.
- Cold attachment votes on current CSR leaves and performs a group-of-leaves rewrite.
  Slot metadata uses copy-on-write, while old edge fragments/generations remain valid.
  Transfer all affected vertex memberships to the new Delta owners; never leave a CSR
  leaf spanning writer anchors, as the SMO capture path assumes one anchor per leaf.
- `topology_mutex_` precedes the native transaction mutex, then writer mutexes. Attachment
  and allocation use exclusive access; established writes/SMO use shared access. Do not
  acquire topology access while already holding the transaction mutex. Snapshot readers
  retain immutable bases and do not acquire the topology gate.
- Keep vertex allocation out of active native snapshots until vertex-lifetime MVCC is
  explicitly implemented; transaction-enabled compilation does not remove this rule.
  No new WAL, crash recovery, UUID mapping, or Java binding is introduced here.

Cold-start validation on macOS (2026-11-04): separate Debug, Release and Debug
transaction build trees each passed 54/54 CTest registrations with no compiler
warnings/errors in the final rebuilds. Array/ART integration matrices passed
12/12, 12/12 and 20/20 respectively. The cold-start executable contains 15 normal
cases / 17 transaction-build cases, including cross-parent TAT rewrites, empty
reopen, reception-page reuse, endpoint validation, voting, giant growth, retained
views and concurrent allocation/updates. Final logs use `verified-*` under
`output/cold-start/`; earlier failed development tests are retained separately.
Linux, crash recovery, JNI integration and throughput improvement were not validated.

### Directed native index (2026-11-04)

- Native import, `bulk_load()` and cold creation are directed. Do not silently symmetrize
  file arcs. Preserve historical legacy-engine import behavior as a separate baseline.
- MCP4 (`io/directional_csr_page.h`) uses a 40-byte header and 32-byte OUT/IN slot.
  Native Index format 8 and Delta format 2 reject older stores. The standalone MCP1
  prototype is unchanged. Use independent output directories for the new format.
- Both directions use existing six-space storage and CSR-owned Delta chains. Store direction
  in the Delta record's former reserved word. One outer vertex generation publishes both
  immutable bases; IN has its own head, stop and timestamp. Update endpoint writers in
  anchor order and roll back both heads on append failure. Transaction commit timestamps
  and watermark filtering must agree across both directions; test public read-your-writes too.
- `read_neighbor(v, direction_t::in)` remains a borrowed base range. Its `_with_delta`
  counterpart returns that direction's chain. Cloning is explicit. Incoming extent frames
  must be included in warmup, giant accounting and configured budgets.
- BYO BFS pull and PageRank read incoming neighbors. WCC must process every logical arc;
  degree-one BFS suppression and half-edge WCC filtering are invalid on directed input.
  Undirected-only kernels use an ephemeral OUT/IN union projection. Native ACGraph-style
  CC currently dispatches to the WCC union kernel; IN scans are not async-prefetched yet.
- Regressions: `test_directed`, `test_byo`, `test_cold_start`, `test_multiscale_graph`, plus
  `tools/validate_vertex_index.sh` for Array/ART and all layouts, including `--txn`.
  `experiments/directed_smoke.sh` records verified RN/YT Release samples in a fresh output
  directory. Retain failed development runs separately from final validation.

Directed-index validation on macOS (2026-11-04): fresh Debug/Release/Debug-transaction
build trees, followed by final incremental rebuilds, passed all 55 CTest registrations
without compiler warnings/errors. Array/ART MTR matrices passed 12/12 Debug, 12/12 Release
and 20/20 transaction cases. An additional cold-start/reopen algorithm case was then
added and both BYO CTest registrations passed again in all three builds. A concurrent
cross-build logger test collided on its shared `/tmp` filename; the failure log is retained,
and a serial transaction rerun passed 55/55. Do not run these full CTest configurations
concurrently until test temporary filenames are isolated. Raw validation logs are in
`output/directed-index/`; measured RN/YT samples are in `output/directed-perf-20261104/`.
This is macOS validation, not a Linux or crash-recovery result.

### Java service and JNI entry points (2026-11-05)

- `include/bw_graph/service/graph_service.h` / `src/graph_service.cc` own named
  native graph stores without changing global storage paths. `bindings/jni/` is
  the JNI adapter; `bindings/java/db/monacgraph/jni/BwGraphJNI.java` mirrors the
  core MonacGraph RustJNI signatures. MonacGraph now has a configurable GraphStorage
  provider; keep its copied Java JNI declarations synchronized.
- `tools/build.sh --jni` enables `BW_GRAPH_BUILD_JNI`, requiring Java 17+ and
  generating headers with `javac -h`, a shared library and a Java JAR. Without
  `--jni` no JDK is required. Use an isolated build directory for `--jni --txn`.
  `test_jni` runs a real JVM, not a C++ mock. `test_graph_service` checks the core.
- Each `<root>/<name>` contains native `adjacency/` and RocksDB `metadata/` for
  binary ID mappings and opaque property blobs. Graph names are restricted,
  graph tokens are registry handles, and internal VIDs are engine allocated.
  Packed directed edge handles use `(src << 32) | dst`. Parallel edges are
  rejected; BOTH deduplicates self loops. Vertex removal cascades over OUT/IN.
- ID forward/reverse maps and the graph-handle lookup table use Folly
  `ConcurrentHashMap`; this is independent of the ART/Array MVVI. Plain F14 is
  used only for the graph-path registry under its open/close mutex. Do not
  describe plain F14 as a concurrent map or conflate container concurrency with
  atomic topology/mapping/property publication.
- Lock order: acquire a graph lease, then graph gate, then deduplicated operation
  stripes in ascending order. Point operations share the gate; edge insertion
  locks the external ID, edge handle and endpoints. Vertex allocation, deletion,
  enumeration, BFS/WCC/SCC and close exclude other storage operations (not pure ID lookup). Graph
  close drains storage operations. ID-only lookup instead hazard-protects both
  registry and map entries and never touches released store resources; maps live
  until those iterators retire. Forward entries are published last, after topology
  and properties; closed/poisoned flags are atomic. The native transaction/page-writer locks
  impose further concurrency limits. No Java multi-operation transaction API.
- Normal close/reopen is supported. A compound mutation error poisons the graph;
  unclean shutdown is detected and reopening refused. The marker is not recovery,
  crash atomicity or cross-store transactional durability. Do not remove the
  marker to bypass an error or silently rebuild an existing directory.
- The JNI adapter uses bounded byte-array inputs and translated Java exceptions.
  Properties and IDs are opaque bytes, including UUIDs/zero bytes. Java result
  arrays copy; native buffer-frame pointers are never exposed to Java. Louvain
  community analytics uses a temporary simple undirected projection, including
  live vertices after deletions. BFS/WCC/SCC include Delta and currently
  use a correctness-first service implementation, not BYO.
- `tools/jni_java.sh` preloads JDK `libjsig` to preserve JVM signal handlers when
  the Folly dylib initializes SIGPIPE. Do not disable `-Xcheck:jni` to hide that
  problem. On this Mac, Unicode-path critical-region warnings also reproduce in
  a Java-only temporary-directory program without loading any Bw-Graph library.
- `tools/benchmark_jni.sh BUILD_DIR [OUTPUT_DIR]` requires Release and retains
  1/4/8/16-thread samples for ID lookups and lookup/property-write mixes. These
  are JNI point-request rates, not edge insertion QPS or a Rust/ART comparison.
- Linux JNI and a remote service remain unverified/unimplemented. The proposed
  gRPC Java/Netty + C++ callback service shares the service core with JNI; see
  README for batching, backpressure, retry and deployment boundaries.
- Validation on this Mac: clean Debug and Release builds and an independent
  transaction-enabled Debug build each passed 57/57 CTest registrations. Final
  service/JNI rebuilds passed in all three configurations after the lookup
  publication changes; the service executable now runs 12 cases across Array
  and ART, including lookup concurrent with erase/reinsert and graph close.
  No compiler warnings/errors were found in these build logs. JVM Unicode-path
  diagnostics described above are retained. Logs and final three-sample JNI
  request measurements are under `output/jni-20261105/`; no Linux, remote-RPC,
  crash-recovery validation is implied. MonacGraph validation is recorded below.


### MonacGraph backend integration (2026-11-05)

- The sibling Java checkout selects `GraphStorage` through `storage.backend`:
  `lsm-community` (default) or `bw-graph`. Named Bw stores use an independent root;
  CSV/JSON property imports resolve mapped file IDs. Directed `.graph` import,
  cold start, reload, clear and HTTP upload preserve the chosen provider.
- Bw JNI now implements community analysis using the pinned Louvain target on
  a simple undirected projection, with mutation exclusion and temporary scratch.
  This differs from the LSM provider's stored community lookup. Neither is BYO.
- MonacGraph tests cover local CRUD/analytics/reopen, mapped imports, concurrency,
  real Gremlin requests, upload replacement, and backend retention. Tests require
  `-Dbwgraph.integration=true`; otherwise they are skipped. Run its
  `scripts/bwgraph.sh test` with JAVA_HOME and optionally BWGRAPH_JNI_LIBRARY.
- Integration exposed and fixed the LSM recovery guard that incorrectly rejected
  cold-created stores with only giant vertices and no bucket files. Its standalone
  Rust regression covers empty/all-giant indexes and rejects missing normal buckets.
  Subgraph JNI now propagates Java callback exceptions; temporary matching files
  are isolated by request. These are MonacGraph-side changes, not Bw disk format changes.
- Java 17 + Netty pooled zlib fails with -Xcheck:jni in a standalone reproduction
  with no graph libraries. Strict integration checks use unpooled network buffers;
  ordinary pooled mode is also checked separately. Production allocator settings
  are unchanged. Preserve libjsig preloading and do not force flat namespaces.
- This remains local macOS JNI integration. No Java multi-operation transactions,
  crash recovery, parallel edges, remote gRPC service, Linux or Windows validation
  is implied. Maintenance operations require quiescent application queries.
- Current native Debug, Release and transaction-enabled CTest runs pass 57/57
  each. Raw integration and build logs are retained under
  `output/monac-integration-20261105/`; do not infer throughput from smoke tests.
- MonacGraph Release/resources and transaction-enabled JNI runs each pass 10/10
  Maven tests. The final Shade JAR passes native resource loading, HTTP Gremlin
  insert/query and graceful shutdown; the Web client builds. Old Assembly output
  dropped Groovy SPI entries and is no longer the launch artifact. Retain Maven
  dependency-overlap and frontend bundle-size warnings; do not claim warning-free
  third-party packaging. Existing LSM native vertex deletion is still unsupported.
