#!/bin/bash

# build.sh - Build script for BwGraph project
# Usage: ./build.sh [-t|--type debug|release] [-c|--clean] [-j|--jobs N] [--compress] [--txn] [--uring] [--jni] [-h|--help]

set -e  # Exit on any error

# Default values
BUILD_TYPE="Debug"  # Default to Debug mode
CLEAN_BUILD=false
RUN_TESTS=true
NEIGHBOR_COMPRESS=false
ENABLE_TXN=false
ENABLE_IO_URING=false
ENABLE_JNI=OFF
BUILD_DIR="build"

# Detect system type and set appropriate CPU core count
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS
    JOBS=$(sysctl -n hw.ncpu 2>/dev/null) || JOBS=4
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    # Linux
    JOBS=$(nproc 2>/dev/null) || JOBS=4
else
    # Fallback for other systems
    JOBS=4
    echo "Warning: Unknown system type, defaulting to 4 parallel jobs"
fi

# Function to show usage
show_usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -t, --type TYPE     Build type: debug|release|relwithdebinfo|minsizerel (default: debug)"
    echo "  --build-dir DIR     Separate build directory (default: build)"
    echo "  -c, --clean         Clean build directory before building"
    echo "  -j, --jobs N        Number of parallel jobs (default: auto-detected)"
    echo "  --compress          Build with PTV-compressed CSR neighbors (default: off)"
    echo "  --txn               Enable transaction support (default: off)"
    echo "  --uring             Enable Linux io_uring page I/O backend (Linux only, default: off)"
    echo "  --jni               Build Java 17 JNI adapter and JVM test (requires JDK)"
    echo "  --no-tests          Skip running tests"
    echo "  -h, --help          Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                  # Build in debug mode (default)"
    echo "  $0 -t release       # Build in release mode"
    echo "  $0 -c -t debug      # Clean build in debug mode"
    echo "  $0 -j 4 -t release  # Build release with 4 parallel jobs"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -t|--type)
            case $2 in
                debug|Debug|DEBUG)
                    BUILD_TYPE="Debug"
                    ;;
                release|Release|RELEASE)
                    BUILD_TYPE="Release"
                    ;;
                relwithdebinfo|RelWithDebInfo|RELWITHDEBINFO)
                    BUILD_TYPE="RelWithDebInfo"
                    ;;
                minsizerel|MinSizeRel|MINSIZEREL)
                    BUILD_TYPE="MinSizeRel"
                    ;;
                *)
                    echo "Error: Invalid build type '$2'"
                    echo "Valid types: debug, release, relwithdebinfo, minsizerel"
                    exit 1
                    ;;
            esac
            shift 2
            ;;
        --build-dir)
            if [[ $# -lt 2 || -z "$2" || "$2" == -* ]]; then
                echo "Error: --build-dir requires a directory"
                exit 1
            fi
            BUILD_DIR=$2
            shift 2
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -j|--jobs)
            JOBS=$2
            shift 2
            ;;
        --no-tests)
            RUN_TESTS=false
            shift
            ;;
        --compress)
            NEIGHBOR_COMPRESS=true
            shift
            ;;
        --txn)
            ENABLE_TXN=true
            shift
            ;;
        --jni)
            ENABLE_JNI=ON
            shift
            ;;
        --uring)
            ENABLE_IO_URING=true
            shift
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            echo "Error: Unknown option '$1'"
            show_usage
            exit 1
            ;;
    esac
done

echo "=== BwGraph Build Script ==="
echo "Build type: $BUILD_TYPE"
echo "Parallel jobs: $JOBS"
echo "Clean build: $CLEAN_BUILD"
echo "Run tests: $RUN_TESTS"
echo "Neighbor compression: $NEIGHBOR_COMPRESS"
echo "Transaction support: $ENABLE_TXN"
echo "io_uring support: $ENABLE_IO_URING"
echo ""

if [[ "$OSTYPE" == "darwin"* && "$ENABLE_IO_URING" = true ]]; then
    echo "Warning: --uring is Linux-only; macOS will use the current pread/pwrite path."
    ENABLE_IO_URING=false
fi

if [ "$NEIGHBOR_COMPRESS" = true ]; then
    CMAKE_NEIGHBOR_COMPRESS="ON"
else
    CMAKE_NEIGHBOR_COMPRESS="OFF"
fi

if [ "$ENABLE_TXN" = true ]; then
    CMAKE_ENABLE_TXN="ON"
else
    CMAKE_ENABLE_TXN="OFF"
fi

if [ "$ENABLE_IO_URING" = true ]; then
    CMAKE_ENABLE_IO_URING="ON"
else
    CMAKE_ENABLE_IO_URING="OFF"
fi

# Resolve before cleaning; never reuse a copied build tree or remove source/data directories.
SOURCE_DIR=$(pwd -P)
if [[ ! -f "$SOURCE_DIR/CMakeLists.txt" || ! -f "$SOURCE_DIR/tools/build.sh" ]]; then
    echo "Error: run this script from the repository root"
    exit 1
fi
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "Error: --jobs must be a positive integer"
    exit 1
fi
mkdir -p "$BUILD_DIR"
BUILD_DIR=$(cd "$BUILD_DIR" && pwd -P)
if [[ "$BUILD_DIR" == "$SOURCE_DIR" || "$SOURCE_DIR" == "$BUILD_DIR/"* || "$BUILD_DIR" == / ]]; then
    echo "Error: build directory must not contain the source tree"
    exit 1
fi
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    CACHED_SOURCE=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt")
    if [[ "$CACHED_SOURCE" != "$SOURCE_DIR" ]]; then
        echo "Error: build cache belongs to another source tree; choose a fresh --build-dir"
        exit 1
    fi
fi
if [ "$CLEAN_BUILD" = true ]; then
    # Only delete a build tree that CMake identifies as belonging to this checkout.
    if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
        rm -rf -- "$BUILD_DIR"
    elif [[ -n "$(ls -A "$BUILD_DIR")" ]]; then
        echo "Error: refusing to clean a nonempty directory without this project's CMake cache"
        exit 1
    fi
fi
# Always configure so changed CMake files and globbed sources are discovered.
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBWGRAPH_NEIGHBOR_COMPRESS="$CMAKE_NEIGHBOR_COMPRESS" \
    -DBW_GRAPH_ENABLE_TRANSACTION="$CMAKE_ENABLE_TXN" \
    -DBW_GRAPH_ENABLE_IO_URING="$CMAKE_ENABLE_IO_URING" \
    -DBW_GRAPH_BUILD_JNI="$ENABLE_JNI"
cd "$BUILD_DIR"

# Build the project
echo "Building project with $JOBS parallel jobs..."
make -j$JOBS

# Run tests if enabled
if [ "$RUN_TESTS" = true ]; then
    echo "Running tests..."
    if ! ctest --output-on-failure; then
        echo "Warning: Some tests failed"
        exit 1
    fi
else
    echo "Skipping tests (--no-tests specified)"
fi

echo ""
echo "=== Build completed successfully! ==="
echo "Build type: $BUILD_TYPE"
echo "Neighbor compression: $NEIGHBOR_COMPRESS"
echo "Transaction support: $ENABLE_TXN"
echo "io_uring support: $ENABLE_IO_URING"
echo "Executables are in: $(pwd)/bin/"
echo ""

# Show some useful information
if [ "$BUILD_TYPE" = "Debug" ]; then
    echo "Debug build completed. You can now use debuggers:"
    echo "  GDB: gdb ./bin/your_executable"
    echo "  LLDB: lldb ./bin/your_executable"
elif [ "$BUILD_TYPE" = "Release" ]; then
    echo "Release build completed with optimizations enabled."
fi
