#!/bin/bash
# Verify IO correctness of kvcache data file
# The data file is generated with deterministic pattern:
# each 8-byte position stores its file offset (little-endian uint64)
#
# Usage: verify_io_correctness.sh [OPTIONS]

set -e

# ===== Default Configuration =====
DATA_FILE="/mnt/nvme4/kvcache_tensor.bin"
BLOCK_SIZE=16384          # 16KB, matches kvcache.sh default
COUNT=0                   # 0 = verify all blocks
SEED=42                   # Random seed for sampling mode
SAMPLE_RATIO=0            # 0 = full verification; >0 = sample this fraction (e.g. 0.01 = 1%)
CHECK_ALIGNMENT=true      # Whether to check O_DIRECT alignment requirements

# ===== Usage =====

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Verify IO correctness of the kvcache deterministic data file.
Each 8-byte position should contain its file offset (little-endian uint64).

Options:
  -f, --file <path>       Data file path [default: ${DATA_FILE}]
  -b, --block-size <B>    Block size in bytes [default: ${BLOCK_SIZE}]
  -c, --count <N>         Number of blocks to verify (0 = all) [default: ${COUNT}]
  -s, --sample <ratio>    Sample ratio for random verification (0 = full, 0.01 = 1%) [default: ${SAMPLE_RATIO}]
      --seed <N>          Random seed for sampling [default: ${SEED}]
  -h, --help              Show this help message

Verification Modes:
  Full mode (default):     Verify every block sequentially
  Count mode:              Verify first N blocks (--count N)
  Sample mode:             Randomly sample blocks (--sample 0.01)

Examples:
  # Full verification of default data file
  sudo $(basename "$0")

  # Verify first 100 blocks with 64KB block size
  sudo $(basename "$0") --block-size 65536 --count 100

  # Random sample 1% of blocks
  sudo $(basename "$0") --sample 0.01

  # Custom file path with sampling
  sudo $(basename "$0") --file /data/test.bin --sample 0.05 --seed 123
EOF
}

# ===== Parse Options =====

while [[ $# -gt 0 ]]; do
    case "$1" in
        -f|--file)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --file requires a file path"
                usage
                exit 1
            fi
            DATA_FILE="$1"
            shift
            ;;
        -b|--block-size)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --block-size requires a number"
                usage
                exit 1
            fi
            BLOCK_SIZE="$1"
            shift
            ;;
        -c|--count)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --count requires a number"
                usage
                exit 1
            fi
            COUNT="$1"
            shift
            ;;
        -s|--sample)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --sample requires a ratio"
                usage
                exit 1
            fi
            SAMPLE_RATIO="$1"
            shift
            ;;
        --seed)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --seed requires a number"
                usage
                exit 1
            fi
            SEED="$1"
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

# ===== Validate =====

if [[ ! -f "${DATA_FILE}" ]]; then
    echo "Error: Data file not found: ${DATA_FILE}"
    exit 1
fi

if ! [[ "${BLOCK_SIZE}" =~ ^[0-9]+$ ]] || [[ "${BLOCK_SIZE}" -le 0 ]] || [[ $((BLOCK_SIZE % 8)) -ne 0 ]]; then
    echo "Error: --block-size must be a positive integer multiple of 8"
    exit 1
fi

if ! [[ "${COUNT}" =~ ^[0-9]+$ ]]; then
    echo "Error: --count must be a non-negative integer"
    exit 1
fi

# ===== Verification =====

FILE_SIZE=$(stat -c%s "${DATA_FILE}")
TOTAL_BLOCKS=$((FILE_SIZE / BLOCK_SIZE))

if [[ ${TOTAL_BLOCKS} -eq 0 ]]; then
    echo "Error: File is smaller than one block (file=${FILE_SIZE}, block_size=${BLOCK_SIZE})"
    exit 1
fi

# Determine verification mode
if (( $(echo "${SAMPLE_RATIO} > 0" | bc -l) )); then
    MODE="sample"
    VERIFY_COUNT=$(( TOTAL_BLOCKS * $(echo "${SAMPLE_RATIO}" | bc -l | awk '{printf "%d", $1}') ))
    if [[ ${VERIFY_COUNT} -lt 1 ]]; then
        VERIFY_COUNT=1
    fi
    if [[ ${COUNT} -gt 0 ]] && [[ ${COUNT} -lt ${VERIFY_COUNT} ]]; then
        VERIFY_COUNT=${COUNT}
    fi
    echo "=== Sample Verification ==="
    echo "Sample ratio: ${SAMPLE_RATIO} (${VERIFY_COUNT} of ${TOTAL_BLOCKS} blocks)"
elif [[ ${COUNT} -gt 0 ]]; then
    MODE="count"
    VERIFY_COUNT=${COUNT}
    if [[ ${VERIFY_COUNT} -gt ${TOTAL_BLOCKS} ]]; then
        VERIFY_COUNT=${TOTAL_BLOCKS}
    fi
    echo "=== Count Verification ==="
    echo "Verifying first ${VERIFY_COUNT} of ${TOTAL_BLOCKS} blocks"
else
    MODE="full"
    VERIFY_COUNT=${TOTAL_BLOCKS}
    echo "=== Full Verification ==="
    echo "Verifying all ${TOTAL_BLOCKS} blocks"
fi

echo "File: ${DATA_FILE}"
echo "File size: ${FILE_SIZE} bytes ($(( FILE_SIZE / 1024 / 1024 ))MB)"
echo "Block size: ${BLOCK_SIZE} bytes"
echo "Blocks to verify: ${VERIFY_COUNT}"
echo ""

# Run verification via Python
python3 -c "
import struct, sys, random, time, os

data_file = '${DATA_FILE}'
block_size = ${BLOCK_SIZE}
file_size = ${FILE_SIZE}
total_blocks = ${TOTAL_BLOCKS}
verify_count = ${VERIFY_COUNT}
mode = '${MODE}'
seed = ${SEED}

# Try to use numpy for vectorized comparison (much faster)
try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

# Large sequential read buffer for full/count mode
READ_SIZE = 4 * 1024 * 1024  # 4MB

def verify_chunk(data, base_offset, error_limit=20):
    \"\"\"Verify a chunk of data using batch unpack. Returns (error_count, error_details).\"\"\"
    n = len(data) // 8
    if n == 0:
        return 0, []

    if HAS_NUMPY:
        # Vectorized: ~10-50x faster than pure Python loop
        arr = np.frombuffer(data[:n*8], dtype='<u8')
        expected = np.arange(base_offset, base_offset + n * 8, 8, dtype=np.uint64)
        mismatches = np.flatnonzero(arr != expected)
        error_count = len(mismatches)
        details = []
        for idx in mismatches[:error_limit]:
            pos = base_offset + int(idx) * 8
            details.append((pos // block_size, pos, int(expected[idx]), int(arr[idx])))
        return error_count, details
    else:
        # Batch struct.unpack: ~100-200x faster than per-8-byte unpack
        values = struct.unpack_from(f'<{n}Q', data)
        error_count = 0
        details = []
        for i in range(n):
            expected = base_offset + i * 8
            if values[i] != expected:
                error_count += 1
                if len(details) < error_limit:
                    pos = base_offset + i * 8
                    details.append((pos // block_size, pos, expected, values[i]))
        return error_count, details

# Determine which blocks to verify
if mode == 'sample':
    random.seed(seed)
    block_ids = sorted(random.sample(range(total_blocks), min(verify_count, total_blocks)))
elif mode == 'count':
    block_ids = list(range(verify_count))
else:
    block_ids = list(range(total_blocks))

errors = []
error_blocks_set = set()
bytes_verified = 0
start_time = time.time()
accel_tag = 'numpy' if HAS_NUMPY else 'batch-struct'

if mode == 'sample':
    # Sample mode: random access, seek per block, but use batch unpack
    with open(data_file, 'rb') as f:
        for idx, block_id in enumerate(block_ids):
            offset = block_id * block_size
            f.seek(offset)
            data = f.read(block_size)
            if not data:
                continue

            err_count, err_details = verify_chunk(data, offset)
            if err_count > 0:
                errors.extend(err_details)
                error_blocks_set.add(block_id)
                print(f'  Block {block_id} (offset 0x{offset:x}): {err_count} errors')

            bytes_verified += len(data)

            if (idx + 1) % 10000 == 0 or idx == len(block_ids) - 1:
                pct = (idx + 1) * 100 // len(block_ids)
                elapsed = time.time() - start_time
                mb_sec = (bytes_verified / 1024 / 1024) / elapsed if elapsed > 0 else 0
                print(f'\r  Progress: {pct}% ({idx + 1}/{len(block_ids)} blocks, {mb_sec:.1f} MB/s, {accel_tag})', end='', flush=True)
        print()

else:
    # Full/count mode: sequential large-buffer read, much faster
    verify_bytes = verify_count * block_size
    with open(data_file, 'rb') as f:
        offset = 0
        bytes_done = 0
        progress_block_idx = 0
        while bytes_done < verify_bytes:
            to_read = min(READ_SIZE, verify_bytes - bytes_done)
            data = f.read(to_read)
            if not data:
                break

            err_count, err_details = verify_chunk(data, offset)
            if err_count > 0:
                errors.extend(err_details)
                for bid, _, _, _ in err_details:
                    error_blocks_set.add(bid)
                # Report errors per block
                for bid, pos, exp, act in err_details:
                    print(f'  Block {bid} (offset 0x{pos:x}): expected {exp}, got {act}')

            bytes_verified += len(data)
            offset += len(data)
            bytes_done += len(data)

            # Progress update
            current_block = min(offset // block_size, verify_count)
            pct = current_block * 100 // verify_count if verify_count > 0 else 100
            elapsed = time.time() - start_time
            mb_sec = (bytes_verified / 1024 / 1024) / elapsed if elapsed > 0 else 0
            if current_block - progress_block_idx >= 10000 or bytes_done >= verify_bytes:
                print(f'\r  Progress: {pct}% ({current_block}/{verify_count} blocks, {mb_sec:.1f} MB/s, {accel_tag})', end='', flush=True)
                progress_block_idx = current_block
        print()

elapsed = time.time() - start_time
mb_verified = bytes_verified / 1024 / 1024
mb_sec = mb_verified / elapsed if elapsed > 0 else 0

print()
print('=== Verification Result ===')
if errors:
    # Show first 20 error details
    display_errors = errors[:20]
    print(f'FAILED: {len(errors)} error(s) found (showing first {len(display_errors)})')
    for block_id, pos, expected, actual in display_errors:
        print(f'  Offset 0x{pos:x} (block {block_id}): expected {expected}, got {actual}')
    print(f'  Error blocks: {len(error_blocks_set)}')
    print(f'  First error at offset: 0x{errors[0][1]:x}')
    sys.exit(1)
else:
    print(f'PASSED: All {verify_count} blocks verified successfully')
    print(f'  Bytes verified: {bytes_verified} ({mb_verified:.1f} MB)')
    print(f'  Throughput: {mb_sec:.1f} MB/s')
    print(f'  Acceleration: {accel_tag}')
    print(f'  Time: {elapsed:.2f}s')
    sys.exit(0)
"
