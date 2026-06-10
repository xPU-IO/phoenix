#!/bin/bash
# Load or unload Phoenix kernel module
# Usage: setup_phoenix_module.sh [OPTIONS] [load|unload]

set -e

# ===== Default Configuration =====
NUMA_NODE=-1
PHOENIX_MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../build/module"

# ===== Usage =====

FORCE_UNLOAD=0
DEBUG_MODE=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [COMMAND]

Load or unload the Phoenix (phoenixfs) kernel module.

Options:
  -n, --numa <node>       NUMA node for Phoenix module (-1 for all nodes) [default: all]
  -d, --module-dir <dir>  Directory containing phoenixfs.ko [default: ${PHOENIX_MODULE_DIR}]
  -v, --debug             Enable info-level debug logging (sets phxfs_debug=1)
  -f, --force             Force unload even if module is in use
  -h, --help              Show this help message

Commands:
  load      Load Phoenix module (default)
  unload    Unload Phoenix module

Examples:
  # Load on all NUMA nodes (default)
  sudo $(basename "$0")

  # Load with debug logging enabled
  sudo $(basename "$0") --debug

  # Load on a specific NUMA node only
  sudo $(basename "$0") --numa 0

  # Unload
  sudo $(basename "$0") unload

  # Force unload when module is in use
  sudo $(basename "$0") -f unload
EOF
}

# ===== Functions =====

numa_display() {
    if [[ "$1" == "-1" ]]; then
        echo "all"
    else
        echo "$1"
    fi
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "Error: This script must be run as root"
        exit 1
    fi
}

load_module() {
    if lsmod | grep -q phoenixfs; then
        echo "Phoenix module already loaded"
        current_numa=$(cat /sys/module/phoenixfs/parameters/phxfs_numa_node 2>/dev/null || echo "N/A")
        current_debug=$(cat /sys/module/phoenixfs/parameters/phxfs_debug 2>/dev/null || echo "N/A")
        echo "Current phxfs_numa_node=$(numa_display "${current_numa}") (expected: $(numa_display "${NUMA_NODE}"))"
        echo "Current phxfs_debug=${current_debug} (expected: ${DEBUG_MODE})"
        return 0
    fi

    echo "=== Loading Phoenix module ==="
    # Ensure nvidia driver is initialized
    nvidia-smi &> /dev/null || { echo "Error: nvidia-smi failed"; exit 1; }

    local module_path="${PHOENIX_MODULE_DIR}/phoenixfs.ko"
    if [[ ! -f "${module_path}" ]]; then
        echo "Error: Module not found at ${module_path}"
        exit 1
    fi

    insmod "${module_path}" phxfs_numa_node="${NUMA_NODE}" phxfs_debug="${DEBUG_MODE}"

    # Verify insmod success
    if ! lsmod | grep -q phoenixfs; then
        echo "Error: insmod completed but module not found in lsmod, load may have failed"
        dmesg | tail -20 | grep -iE "phxfs|phoenix|insmod" || true
        exit 1
    fi

    echo "Phoenix module loaded with phxfs_numa_node=$(numa_display "${NUMA_NODE}"), phxfs_debug=${DEBUG_MODE}"

    # Verify module parameters
    local verify_numa
    verify_numa=$(cat /sys/module/phoenixfs/parameters/phxfs_numa_node 2>/dev/null || echo "N/A")
    local verify_debug
    verify_debug=$(cat /sys/module/phoenixfs/parameters/phxfs_debug 2>/dev/null || echo "N/A")
    echo "Verified: phxfs_numa_node=$(numa_display "${verify_numa}"), phxfs_debug=${verify_debug}"

    # Show kernel log
    dmesg | tail -20 | grep "phxfs:" || true
}

unload_module() {
    if ! lsmod | grep -q phoenixfs; then
        echo "Phoenix module is not loaded"
        return 0
    fi

    echo "=== Unloading Phoenix module ==="

    # Try rmmod and capture exit code (disable set -e temporarily)
    local rmmod_rc=0
    rmmod phoenixfs 2>/dev/null || rmmod_rc=$?

    if [[ "${rmmod_rc}" -eq 0 ]]; then
        echo "Phoenix module unloaded successfully"
        return 0
    fi

    # rmmod failed - check if module is in use
    local ref_count
    ref_count=$(lsmod | grep "^phoenixfs" | awk '{print $3}')

    if [[ "${ref_count:-0}" -ne 0 ]]; then
        echo "Warning: Phoenix module is currently in use (refcount=${ref_count})"
        echo "The following processes may be using it:"
        lsmod | grep "^phoenixfs" | awk '{print $4}' | tr ',' '\n' | while read -r dep; do
            echo "  - ${dep}"
        done
        echo ""
        if [[ "${FORCE_UNLOAD}" -eq 1 ]]; then
            echo "Force unload requested, attempting modprobe -r..."
            local modprobe_rc=0
            modprobe -r phoenixfs 2>/dev/null || modprobe_rc=$?
            if [[ "${modprobe_rc}" -eq 0 ]]; then
                echo "Phoenix module force unloaded successfully"
                return 0
            fi
            echo "Error: Force unload failed"
            dmesg | tail -20 | grep -iE "phxfs|phoenix|rmmod" || true
        else
            echo "Please stop the processes above before unloading, or use -f/--force to force unload."
        fi
        return 1
    fi

    # Other rmmod error
    echo "Error: Failed to unload Phoenix module"
    dmesg | tail -20 | grep -iE "phxfs|phoenix|rmmod" || true
    return 1
}

# ===== Parse Options =====

COMMAND=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -n|--numa)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --numa requires a node number"
                usage
                exit 1
            fi
            NUMA_NODE="$1"
            shift
            ;;
        -d|--module-dir)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --module-dir requires a directory path"
                usage
                exit 1
            fi
            PHOENIX_MODULE_DIR="$1"
            shift
            ;;
        -f|--force)
            FORCE_UNLOAD=1
            shift
            ;;
        -v|--debug)
            DEBUG_MODE=1
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

# ===== Main =====
case "${COMMAND:-load}" in
    load)
        check_root
        load_module
        ;;
    unload)
        check_root
        unload_module || exit 1
        ;;
    *)
        echo "Error: Unknown command '${COMMAND}'"
        usage
        exit 1
        ;;
esac
