#!/bin/bash
# Phoenix NVMe RAID0 Environment Setup Script
# Configures 4x NVMe devices as RAID0, mounts XFS, generates data file, loads Phoenix module

set -e

# ===== Configuration =====
NVME_DEVICES=("/dev/nvme4n1" "/dev/nvme5n1" "/dev/nvme6n1" "/dev/nvme7n1")
RAID_DEV="/dev/md0"
MOUNT_POINT="/mnt/nvme4"
DATA_FILE="${MOUNT_POINT}/kvcache_tensor.bin"
DATA_SIZE_GB=20
NUMA_NODE=1
GPU_ID=4
PHOENIX_MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../build/module"

# ===== Functions =====

check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "Error: This script must be run as root"
        exit 1
    fi
}

check_mdadm() {
    if ! command -v mdadm &> /dev/null; then
        echo "Installing mdadm..."
        apt-get install -y mdadm
    fi
}

setup_raid() {
    # Check if RAID already exists
    if mdadm --detail "${RAID_DEV}" &> /dev/null; then
        echo "RAID array ${RAID_DEV} already exists"
        return 0
    fi

    echo "=== Creating RAID0 array ==="
    # Wipe existing filesystem signatures
    for dev in "${NVME_DEVICES[@]}"; do
        echo "Wiping ${dev}..."
        wipefs -a "${dev}" 2>/dev/null || true
    done

    # Create RAID0
    mdadm --create "${RAID_DEV}" --level=0 --raid-devices=${#NVME_DEVICES[@]} "${NVME_DEVICES[@]}"
    echo "RAID0 array created"

    # Wait for array to stabilize
    sleep 2
    cat /proc/mdstat
}

format_and_mount() {
    # Check if already mounted
    if mountpoint -q "${MOUNT_POINT}"; then
        echo "${MOUNT_POINT} is already mounted"
        return 0
    fi

    echo "=== Formatting and mounting ==="
    mkdir -p "${MOUNT_POINT}"

    # Format as XFS if not already
    if ! blkid "${RAID_DEV}" | grep -q "xfs"; then
        mkfs.xfs -f "${RAID_DEV}"
    fi

    # Mount
    mount -o noatime "${RAID_DEV}" "${MOUNT_POINT}"
    echo "Mounted ${RAID_DEV} at ${MOUNT_POINT}"

    # Save config for persistence
    mkdir -p /etc/mdadm
    mdadm --detail --scan > /etc/mdadm/mdadm.conf
    grep -q "${RAID_DEV}" /etc/fstab 2>/dev/null || \
        echo "${RAID_DEV} ${MOUNT_POINT} xfs defaults,noatime 0 0" >> /etc/fstab
}

generate_data_file() {
    if [[ -f "${DATA_FILE}" ]] && [[ $(stat -c%s "${DATA_FILE}" 2>/dev/null || echo 0) -gt $((DATA_SIZE_GB * 1024 * 1024 * 1024 - 1)) ]]; then
        echo "Data file ${DATA_FILE} already exists and has correct size"
        return 0
    fi

    echo "=== Generating ${DATA_SIZE_GB}GB data file ==="
    dd if=/dev/urandom of="${DATA_FILE}" bs=1M count=$((DATA_SIZE_GB * 1024)) status=progress
    echo "Data file generated: ${DATA_FILE}"
}

load_phoenix_module() {
    if lsmod | grep -q phoenixfs; then
        echo "Phoenix module already loaded"
        # Check NUMA parameter
        current_numa=$(cat /sys/module/phoenixfs/parameters/phxfs_numa_node 2>/dev/null || echo "N/A")
        echo "Current phxfs_numa_node=${current_numa} (expected: ${NUMA_NODE})"
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

    insmod "${module_path}" phxfs_numa_node="${NUMA_NODE}"
    echo "Phoenix module loaded with phxfs_numa_node=${NUMA_NODE}"

    # Verify
    dmesg | tail -20 | grep "phxfs:" || true
}

verify_environment() {
    echo ""
    echo "=== Environment Verification ==="
    echo "RAID:       $(mdadm --detail ${RAID_DEV} 2>/dev/null | grep 'Version' || echo 'N/A')"
    echo "Mount:      $(df -h ${MOUNT_POINT} 2>/dev/null | tail -1 || echo 'N/A')"
    echo "Data file:  $(ls -lh ${DATA_FILE} 2>/dev/null || echo 'NOT FOUND')"
    echo "GPU ${GPU_ID}:   $(nvidia-smi -i ${GPU_ID} --query-gpu=name --format=csv,noheader 2>/dev/null || echo 'N/A')"
    echo "Phoenix:    $(lsmod | grep phoenixfs || echo 'NOT LOADED')"
    echo ""

    # Quick native read test
    echo "=== Quick native read test ==="
    local exec_path="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../build/bin/kvcache"
    if [[ -x "${exec_path}" ]]; then
        "${exec_path}" native "${GPU_ID}" \
            "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../benchmarks/kvcache/traces/quality.txt" \
            16384 "${DATA_FILE}" 2>&1 | tail -8
    else
        echo "kvcache binary not found at ${exec_path}"
    fi
}

# ===== Main =====
case "${1:-all}" in
    raid)
        check_root
        check_mdadm
        setup_raid
        format_and_mount
        ;;
    data)
        check_root
        generate_data_file
        ;;
    module)
        check_root
        load_phoenix_module
        ;;
    verify)
        verify_environment
        ;;
    all)
        check_root
        check_mdadm
        setup_raid
        format_and_mount
        generate_data_file
        load_phoenix_module
        verify_environment
        ;;
    cleanup)
        check_root
        echo "=== Cleaning up ==="
        umount "${MOUNT_POINT}" 2>/dev/null || true
        mdadm --stop "${RAID_DEV}" 2>/dev/null || true
        for dev in "${NVME_DEVICES[@]}"; do
            wipefs -a "${dev}" 2>/dev/null || true
        done
        rmmod phoenixfs 2>/dev/null || true
        echo "Cleanup done"
        ;;
    *)
        echo "Usage: $0 {all|raid|data|module|verify|cleanup}"
        echo "  all     - Setup everything (default)"
        echo "  raid    - Create RAID0 and mount only"
        echo "  data    - Generate data file only"
        echo "  module  - Load Phoenix module only"
        echo "  verify  - Verify environment"
        echo "  cleanup - Unmount, stop RAID, unload module"
        exit 1
        ;;
esac
