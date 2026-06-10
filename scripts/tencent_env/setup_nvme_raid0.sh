#!/bin/bash
# Setup NVMe devices as RAID0, format XFS, and mount
# Usage: setup_nvme_raid0.sh [OPTIONS] [setup|cleanup]

set -e

# ===== Default Configuration =====
DEFAULT_DEVICES="nvme4n1 nvme5n1 nvme6n1 nvme7n1"
RAID_DEV="/dev/md0"
MOUNT_POINT="/mnt/nvme4"

# ===== Usage =====

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [COMMAND]

Setup NVMe devices as RAID0, format XFS filesystem, and mount.

Options:
  -d, --devices <dev1 dev2 ...>   NVMe device names (space-separated, auto-prefixed with /dev/)
                                  [default: ${DEFAULT_DEVICES}]
  -r, --raid-dev <path>           RAID array device path [default: ${RAID_DEV}]
  -m, --mount <dir>               Mount point directory [default: ${MOUNT_POINT}]
  -h, --help                      Show this help message

Commands:
  setup     Create RAID0, format XFS, and mount (default)
  cleanup   Unmount, stop RAID, and wipe devices

Examples:
  # Default setup with 4 NVMe devices
  sudo $(basename "$0")

  # Custom NVMe devices
  sudo $(basename "$0") --devices "nvme0n1 nvme1n1 nvme2n1 nvme3n1"

  # Custom RAID device and mount point
  sudo $(basename "$0") --raid-dev /dev/md1 --mount /mnt/fast

  # Cleanup
  sudo $(basename "$0") cleanup
EOF
}

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

do_cleanup() {
    echo "=== Cleaning up ==="
    umount "${MOUNT_POINT}" 2>/dev/null || true
    mdadm --stop "${RAID_DEV}" 2>/dev/null || true
    for dev in "${NVME_DEVICES[@]}"; do
        wipefs -a "${dev}" 2>/dev/null || true
    done
    echo "Cleanup done"
}

# ===== Parse Options =====

NVME_DEVICES=()
COMMAND=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--devices)
            shift
            if [[ $# -eq 0 || "$1" == -* ]]; then
                echo "Error: --devices requires a space-separated list of device names"
                usage
                exit 1
            fi
            NVME_DEVICES=()
            while [[ $# -gt 0 && "$1" != -* ]]; do
                if [[ "$1" == /dev/* ]]; then
                    NVME_DEVICES+=("$1")
                else
                    NVME_DEVICES+=("/dev/$1")
                fi
                shift
            done
            ;;
        -r|--raid-dev)
            shift
            if [[ $# -eq 0 ]]; then
                echo "Error: --raid-dev requires a path"
                usage
                exit 1
            fi
            RAID_DEV="$1"
            shift
            ;;
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

# Apply default devices if not specified via --devices
if [[ ${#NVME_DEVICES[@]} -eq 0 ]]; then
    for dev in ${DEFAULT_DEVICES}; do
        NVME_DEVICES+=("/dev/${dev}")
    done
fi

# Validate parameters
if [[ ${#NVME_DEVICES[@]} -lt 1 ]]; then
    echo "Error: At least one NVMe device is required"
    exit 1
fi

# ===== Main =====
case "${COMMAND:-setup}" in
    setup)
        check_root
        check_mdadm
        setup_raid
        format_and_mount
        ;;
    cleanup)
        check_root
        do_cleanup
        ;;
    *)
        echo "Error: Unknown command '${COMMAND}'"
        usage
        exit 1
        ;;
esac
