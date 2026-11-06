# Bw-Graph Dependencies

Complete installation commands for a fresh environment, version baselines, platform notes, and validation results are maintained in Sections 2–3 of [AGENTS.md](AGENTS.md).

| Dependency | Installation policy |
| --- | --- |
| C++20 compiler / Make / Git | macOS Command Line Tools or Linux system tools |
| CMake / pkg-config | Homebrew; the dependency chain requires CMake >= 3.21 |
| folly | **Homebrew** |
| RocksDB | **Git sources**, v10.4.2; install into `/usr/local` |
| oneTBB | **Git sources**, v2022.2.0; install into `/usr/local` |
| KaMinPar | **Preserved source tree from the migration**; enable 64-bit node IDs, edge IDs, and weights, use source-built TBB, and install into `/usr/local` |
| kassert | Pinned Git commit; used when building KaMinPar |
| indicators | Git v2.3; install headers and the CMake package into `/usr/local` |
| gflags / glog / yaml-cpp / nlohmann-json / googletest | Homebrew |
| google-sparsehash / hwloc | Homebrew; hwloc is a runtime dependency of source-built oneTBB's tbbbind |
| snappy / lz4 / zstd / zlib / bzip2 | RocksDB compression dependencies; see the full installation workflow for their sources |
| libomp | OpenMP support on macOS; configure for the selected compiler on Linux |
| POSIX Threads | Provided by the operating system |
| liburing | Install `liburing-dev` when enabling `--uring` on Linux |
| SIMDCompressionAndIntersection | Optional on Linux; CMake enables the PForDelta path when detected |

Keep all Git dependency directories under `~/CProjects`, alongside Bw-Graph. Build each dependency in its own `build/` directory, then run `sudo make install`. Do not download dependencies into the main project or install them into a project-private `.deps` directory.

Build in this order: oneTBB → RocksDB / indicators → KaMinPar → Bw-Graph. Prepare kassert before configuring KaMinPar. After upgrading oneTBB, rebuild and reinstall KaMinPar, then rebuild Bw-Graph.

The complete Ubuntu environment has not been verified during this migration. Passing macOS tests does not establish Ubuntu compatibility. Continue to install folly through Homebrew; do not assume the system package repositories provide a usable `libfolly-dev` package.
