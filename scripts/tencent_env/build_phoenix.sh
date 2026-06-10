#!/bin/bash
# Build Phoenix from source
# Usage: build_phoenix.sh [OPTIONS] [COMMAND]

set -e

# ===== Default Configuration =====
PHOENIX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../.."
BUILD_DIR="${PHOENIX_ROOT}/build"
BUILD_TYPE="Release"
NVIDIA_ARCHS="90"
JOBS=$(nproc)
NO_MODULE=0
NO_CUDA=0
CLEAN=0

# ===== Color Output =====
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# ===== Usage =====

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [COMMAND]

Build Phoenix from source (kernel module + userspace library + benchmarks).

Options:
  -b, --build-dir <dir>    Build output directory [default: ${BUILD_DIR}]
  -t, --type <type>        Build type: Release or Debug [default: ${BUILD_TYPE}]
  -a, --arch <archs>       NVIDIA compute architectures (comma-separated) [default: ${NVIDIA_ARCHS}]
  -j, --jobs <N>           Number of parallel jobs [default: ${JOBS}]
      --no-module          Skip kernel module build
      --no-cuda            Skip CUDA support (library only, no GPU)
  -c, --clean              Clean build (remove build directory first)
  -h, --help               Show this help message

Commands:
  build      Full build (default): configure + compile
  configure  Run CMake configure only (no compile)
  module     Build kernel module only
  library    Build userspace library only
  clean      Clean build directory

Examples:
  # Full build with defaults
  $(basename "$0")

  # Debug build for development
  $(basename "$0") --type Debug --clean

  # Build with specific GPU architectures
  $(basename "$0") --arch "80;90"

  # Build kernel module only (no CUDA library)
  $(basename "$0") --no-cuda module

  # Clean build directory
  $(basename "$0") clean
EOF
}

# ===== Dependency Checks =====

check_dependencies() {
    echo "=== Checking build dependencies ==="
    local missing=0

    # CMake
    if command -v cmake &> /dev/null; then
        local cmake_ver
        cmake_ver=$(cmake --version | head -1 | awk '{print $3}')
        log_info "cmake: ${cmake_ver}"
    else
        log_error "cmake >= 3.18 not found"
        missing=1
    fi

    # GCC / G++
    if command -v gcc &> /dev/null; then
        log_info "gcc: $(gcc --version | head -1 | awk '{print $NF}')"
    else
        log_error "gcc not found"
        missing=1
    fi

    if command -v g++ &> /dev/null; then
        log_info "g++: $(g++ --version | head -1 | awk '{print $NF}')"
    else
        log_error "g++ not found"
        missing=1
    fi

    # Kernel headers (needed for module build)
    if [[ "${NO_MODULE}" -eq 0 ]]; then
        local kver
        kver=$(uname -r)
        if [[ -d "/lib/modules/${kver}/build" ]]; then
            log_info "kernel headers: ${kver} (/lib/modules/${kver}/build)"
        else
            log_error "kernel headers not found for ${kver} (install linux-headers-$(uname -r))"
            missing=1
        fi
    fi

    # CUDA
    if [[ "${NO_CUDA}" -eq 0 ]]; then
        if command -v nvcc &> /dev/null; then
            local cuda_ver
            cuda_ver=$(nvcc --version | grep release | awk '{print $5}' | sed 's/,//')
            log_info "CUDA: ${cuda_ver}"
        else
            log_error "nvcc not found (CUDA >= 12.4 required)"
            missing=1
        fi
    fi

    echo ""
    if [[ "${missing}" -eq 1 ]]; then
        log_error "Missing required dependencies, please install them first"
        exit 1
    fi
    log_info "All dependencies satisfied"
}

# ===== Build Functions =====

do_clean() {
    if [[ -d "${BUILD_DIR}" ]]; then
        echo "=== Cleaning build directory ==="
        rm -rf "${BUILD_DIR}"
        log_info "Build directory removed: ${BUILD_DIR}"
    else
        log_info "Build directory does not exist, nothing to clean"
    fi
}

do_configure() {
    echo "=== Configuring Phoenix ==="
    mkdir -p "${BUILD_DIR}"

    local cmake_args=(
        -B "${BUILD_DIR}"
        -S "${PHOENIX_ROOT}"
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
        -DCMAKE_CUDA_COMPILER="/usr/local/cuda/bin/nvcc"
        -DCMAKE_CUDA_ARCHITECTURES="${NVIDIA_ARCHS}"
        -Dnvidia_archs="${NVIDIA_ARCHS}"
    )

    if [[ "${NO_MODULE}" -eq 1 ]]; then
        cmake_args+=(-Dno_module=true)
        log_info "Kernel module build: DISABLED"
    fi

    if [[ "${NO_CUDA}" -eq 1 ]]; then
        cmake_args+=(-Dno_cuda=true)
        log_info "CUDA support: DISABLED"
    fi

    log_info "Build type: ${BUILD_TYPE}"
    log_info "NVIDIA archs: ${NVIDIA_ARCHS}"
    log_info "Build dir: ${BUILD_DIR}"
    log_info "Jobs: ${JOBS}"
    echo ""

    cmake "${cmake_args[@]}"

    log_info "Configure completed"
}

do_build() {
    echo "=== Building Phoenix ==="
    mkdir -p "${BUILD_DIR}"

    # Configure if not done yet
    if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        do_configure
        echo ""
    fi

    local cmake_args=(
        --build "${BUILD_DIR}"
        -j "${JOBS}"
    )

    cmake "${cmake_args[@]}"

    echo ""
    log_info "Build completed successfully"
    print_build_summary
}

do_build_module() {
    echo "=== Building kernel module only ==="
    mkdir -p "${BUILD_DIR}"

    if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        do_configure
        echo ""
    fi

    cmake --build "${BUILD_DIR}" --target modules -j "${JOBS}"

    echo ""
    if [[ -f "${BUILD_DIR}/module/phoenixfs.ko" ]]; then
        log_info "Kernel module built: ${BUILD_DIR}/module/phoenixfs.ko"
    else
        log_error "Kernel module not found after build"
        exit 1
    fi
}

do_build_library() {
    echo "=== Building userspace library only ==="
    mkdir -p "${BUILD_DIR}"

    if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        do_configure
        echo ""
    fi

    cmake --build "${BUILD_DIR}" --target libphoenix -j "${JOBS}"

    echo ""
    local lib_path
    lib_path=$(find "${BUILD_DIR}" -name "libphoenix.so" -type f 2>/dev/null | head -1)
    if [[ -n "${lib_path}" ]]; then
        log_info "Library built: ${lib_path}"
    else
        log_warn "Shared library not found, checking for static library..."
        lib_path=$(find "${BUILD_DIR}" -name "libphoenix.a" -type f 2>/dev/null | head -1)
        if [[ -n "${lib_path}" ]]; then
            log_info "Static library built: ${lib_path}"
        else
            log_error "Library not found after build"
            exit 1
        fi
    fi
}

print_build_summary() {
    echo "=== Build Summary ==="

    # Kernel module
    if [[ -f "${BUILD_DIR}/module/phoenixfs.ko" ]]; then
        log_info "Kernel module: ${BUILD_DIR}/module/phoenixfs.ko"
    else
        log_warn "Kernel module: NOT BUILT"
    fi

    # Shared library
    local lib_path
    lib_path=$(find "${BUILD_DIR}" -name "libphoenix.so" -type f 2>/dev/null | head -1)
    if [[ -n "${lib_path}" ]]; then
        log_info "Shared library: ${lib_path}"
    else
        lib_path=$(find "${BUILD_DIR}" -name "libphoenix.a" -type f 2>/dev/null | head -1)
        if [[ -n "${lib_path}" ]]; then
            log_info "Static library: ${lib_path}"
        else
            log_warn "Library: NOT BUILT"
        fi
    fi

    echo ""
    echo "Next steps:"
    echo "  1. Load kernel module:  sudo $(dirname "$0")/setup_phoenix_module.sh"
}

# ===== Parse Options =====

COMMAND=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--build-dir)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --build-dir requires a directory path"
                usage
                exit 1
            fi
            BUILD_DIR="$1"
            shift
            ;;
        -t|--type)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --type requires a build type (Release|Debug)"
                usage
                exit 1
            fi
            BUILD_TYPE="$1"
            shift
            ;;
        -a|--arch)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --arch requires architecture string"
                usage
                exit 1
            fi
            NVIDIA_ARCHS="$1"
            shift
            ;;
        -j|--jobs)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --jobs requires a number"
                usage
                exit 1
            fi
            JOBS="$1"
            shift
            ;;
        --no-module)
            NO_MODULE=1
            shift
            ;;
        --no-cuda)
            NO_CUDA=1
            shift
            ;;
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "Error: Unknown option $1"
            usage
            exit 1
            ;;
        *)
            COMMAND="$1"
            shift
            break
            ;;
    esac
done

# Validate build type
if [[ "${BUILD_TYPE}" != "Release" && "${BUILD_TYPE}" != "Debug" ]]; then
    log_error "Invalid build type: ${BUILD_TYPE} (must be Release or Debug)"
    exit 1
fi

# ===== Main =====

# Clean if requested
if [[ "${CLEAN}" -eq 1 && "${COMMAND}" != "clean" ]]; then
    do_clean
    echo ""
fi

case "${COMMAND:-build}" in
    build)
        check_dependencies
        echo ""
        do_build
        ;;
    configure)
        check_dependencies
        echo ""
        do_configure
        ;;
    module)
        check_dependencies
        echo ""
        do_build_module
        ;;
    library)
        check_dependencies
        echo ""
        do_build_library
        ;;
    clean)
        do_clean
        ;;
    *)
        echo "Error: Unknown command '${COMMAND}'"
        usage
        exit 1
        ;;
esac
