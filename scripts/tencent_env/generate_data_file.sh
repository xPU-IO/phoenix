#!/bin/bash
# Generate kvcache test data file
# Usage: generate_data_file.sh [OPTIONS]

set -e

# ===== Default Configuration =====
MOUNT_POINT="/mnt/nvme4"
DATA_SIZE_GB=20
DATA_FILE=""

# ===== Usage =====

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Generate a deterministic data file for kvcache benchmarking.
Each 8-byte position stores its file offset (little-endian uint64),
enabling IO correctness verification.

Options:
  -m, --mount <dir>       Mount point directory [default: ${MOUNT_POINT}]
  -s, --size <GB>         Data file size in GB [default: ${DATA_SIZE_GB}]
  -o, --output <path>     Output file path [default: ${MOUNT_POINT}/kvcache_tensor.bin]
  -h, --help              Show this help message

Examples:
  # Generate 20GB data file at default location
  sudo $(basename "$0")

  # Generate 50GB data file
  sudo $(basename "$0") --size 50

  # Custom output path
  sudo $(basename "$0") --output /data/test_tensor.bin --size 10
EOF
}

# ===== Functions =====

generate_data_file() {
    local data_file="${DATA_FILE:-${MOUNT_POINT}/kvcache_tensor.bin}"
    local expected_size=$((DATA_SIZE_GB * 1024 * 1024 * 1024))

    # Check if existing file is valid (correct size + deterministic pattern)
    if [[ -f "${data_file}" ]]; then
        local actual_size=$(stat -c%s "${data_file}" 2>/dev/null || echo 0)
        if [[ "${actual_size}" -eq "${expected_size}" ]]; then
            # Quick pattern check: verify first 8 bytes == 0x0000000000000000
            # and last 8 bytes == (expected_size - 8) in little-endian
            local first_8=$(od -An -tu8 -N8 "${data_file}" | tr -d ' ')
            local last_offset=$((expected_size - 8))
            local last_8=$(od -An -tu8 -j"${last_offset}" -N8 "${data_file}" | tr -d ' ')
            if [[ "${first_8}" == "0" ]] && [[ "${last_8}" == "${last_offset}" ]]; then
                echo "Data file ${data_file} already exists with correct size and deterministic pattern"
                return 0
            else
                echo "Data file ${data_file} exists but pattern mismatch, regenerating..."
            fi
        else
            echo "Data file ${data_file} exists but size mismatch (${actual_size} != ${expected_size}), regenerating..."
        fi
    fi

    echo "=== Generating ${DATA_SIZE_GB}GB deterministic data file ==="
    echo "Pattern: each 8-byte position stores its file offset (little-endian uint64)"

    # Use Python to generate deterministic data: every 8 bytes = file offset
    python3 -c "
import struct, sys, os

size = ${expected_size}
path = '${data_file}'
chunk_size = 256 * 1024 * 1024  # 256MB chunks for progress

with open(path, 'wb') as f:
    written = 0
    while written < size:
        # Build a chunk of data
        buf = bytearray(min(chunk_size, size - written))
        # Fill buf: every 8 bytes = (written + i)
        for i in range(0, len(buf), 8):
            struct.pack_into('<Q', buf, i, written + i)
        f.write(buf)
        written += len(buf)
        pct = written * 100 // size
        print(f'\r  Progress: {pct}% ({written // (1024*1024)}MB / {size // (1024*1024)}MB)', end='', flush=True)
    print()

print('Data file generated: ' + path, file=sys.stderr)
"
    echo "Data file generated: ${data_file}"
}

# ===== Parse Options =====

while [[ $# -gt 0 ]]; do
    case "$1" in
        -m|--mount)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --mount requires a directory path"
                usage
                exit 1
            fi
            MOUNT_POINT="$1"
            shift
            ;;
        -s|--size)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --size requires a number"
                usage
                exit 1
            fi
            DATA_SIZE_GB="$1"
            shift
            ;;
        -o|--output)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --output requires a file path"
                usage
                exit 1
            fi
            DATA_FILE="$1"
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
            echo "Error: Unexpected argument '$1'"
            usage
            exit 1
            ;;
    esac
done

# Validate parameters
if ! [[ "${DATA_SIZE_GB}" =~ ^[0-9]+$ ]] || [[ "${DATA_SIZE_GB}" -le 0 ]]; then
    echo "Error: --size must be a positive integer"
    exit 1
fi

# ===== Main =====
generate_data_file
