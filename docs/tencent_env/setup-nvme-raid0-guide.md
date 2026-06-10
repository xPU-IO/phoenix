# Phoenix NVMe RAID0 存储配置指南

本文档介绍如何使用 `scripts/tencent_env/setup_nvme_raid0.sh` 脚本将多块 NVMe 设备配置为 RAID0 阵列、格式化为 XFS 文件系统并挂载。

## 前提条件

- 至少一块 NVMe 设备可用（多块可组建 RAID0 以获得更高带宽）
- 需要 **root 权限**（`sudo`）
- 已安装 `mdadm`（脚本会自动检测并安装）
- 已安装 `xfsprogs`（用于 XFS 格式化）


## 快速开始

```bash
# 使用默认配置（nvme4n1~nvme7n1 组建 RAID0，挂载到 /mnt/nvme4）
sudo ./scripts/tencent_env/setup_nvme_raid0.sh

# 清理 RAID 阵列
sudo ./scripts/tencent_env/setup_nvme_raid0.sh cleanup
```

## 命令格式

```bash
sudo ./scripts/tencent_env/setup_nvme_raid0.sh [OPTIONS] [COMMAND]
```

### 命令（COMMAND）

| 命令 | 说明 |
|------|------|
| `setup` | **默认**。创建 RAID0、格式化 XFS、挂载 |
| `cleanup` | 卸载、停止 RAID、擦除设备签名 |

### 选项（OPTIONS）

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `-d, --devices <dev1 dev2 ...>` | `nvme4n1 nvme5n1 nvme6n1 nvme7n1` | NVMe 设备名称列表（空格分隔），自动添加 `/dev/` 前缀 |
| `-r, --raid-dev <path>` | `/dev/md0` | RAID 阵列设备路径 |
| `-m, --mount <dir>` | `/mnt/nvme4` | 挂载点目录 |
| `-h, --help` | - | 显示帮助信息 |

## 使用示例

### 默认配置

```bash
# 使用 4 块 NVMe 设备组建 RAID0，挂载到 /mnt/nvme4
sudo ./scripts/tencent_env/setup_nvme_raid0.sh
```

### 自定义 NVMe 设备

```bash
# 指定其他 NVMe 设备（无需加 /dev/ 前缀，脚本会自动添加）
sudo ./scripts/tencent_env/setup_nvme_raid0.sh --devices "nvme0n1 nvme1n1 nvme2n1 nvme3n1"

# 也可以使用完整路径
sudo ./scripts/tencent_env/setup_nvme_raid0.sh --devices "/dev/nvme0n1 /dev/nvme1n1"
```

### 自定义 RAID 设备和挂载点

```bash
# 使用 /dev/md1 作为 RAID 设备，挂载到 /mnt/fast
sudo ./scripts/tencent_env/setup_nvme_raid0.sh --raid-dev /dev/md1 --mount /mnt/fast
```

### 单盘模式

```bash
# 仅使用一块 NVMe 设备（不组建 RAID，但仍通过 mdadm 管理）
sudo ./scripts/tencent_env/setup_nvme_raid0.sh --devices "nvme0n1"
```

### 清理

```bash
# 卸载文件系统、停止 RAID 阵列、擦除设备签名
sudo ./scripts/tencent_env/setup_nvme_raid0.sh cleanup
```

## Setup 流程详解

脚本执行 `setup` 命令时按以下步骤进行：

1. **Root 权限检查**：非 root 用户直接报错退出
2. **mdadm 检查/安装**：若 `mdadm` 未安装则自动通过 `apt-get` 安装
3. **RAID 创建**（`setup_raid`）：
   - 检查 RAID 阵列是否已存在，已存在则跳过
   - 使用 `wipefs -a` 清除每块设备上的文件系统签名
   - 使用 `mdadm --create` 创建 RAID0 阵列
   - 等待阵列稳定（2 秒），显示 `/proc/mdstat`
4. **格式化与挂载**（`format_and_mount`）：
   - 检查挂载点是否已挂载，已挂载则跳过
   - 创建挂载点目录
   - 若 RAID 设备未格式化为 XFS，执行 `mkfs.xfs -f`
   - 使用 `noatime` 选项挂载
   - 保存 mdadm 配置到 `/etc/mdadm/mdadm.conf`（用于重启后自动组装）
   - 写入 `/etc/fstab` 实现开机自动挂载

## Cleanup 流程详解

脚本执行 `cleanup` 命令时按以下步骤进行：

1. **Root 权限检查**
2. **卸载文件系统**：`umount` 挂载点（失败不退出）
3. **停止 RAID 阵列**：`mdadm --stop`（失败不退出）
4. **擦除设备签名**：对每块设备执行 `wipefs -a`

> **注意**：cleanup 不会从 `/etc/fstab` 和 `/etc/mdadm/mdadm.conf` 中移除相关条目，如需彻底清理请手动编辑。

## 查看系统中的 NVMe 设备

在运行脚本前，建议先确认系统中有哪些 NVMe 设备可用：

```bash
# 列出所有 NVMe 设备
lsblk | grep nvme

# 查看设备详细信息
nvme list

# 查看设备 NUMA 归属（多 NUMA 系统建议选择与 GPU 同 NUMA 的 NVMe）
cat /sys/class/nvme/nvme*/device/numa_node
```

## RAID0 性能说明

| 特性 | 说明 |
|------|------|
| 条带大小 | 默认 512KB（mdadm 默认值） |
| 文件系统 | XFS，挂载选项 `noatime` |
| 容量 | 所有设备容量之和（无冗余） |
| 带宽 | 近似为单设备的 N 倍（N 为设备数） |
| 可靠性 | 任一设备故障将导致全阵列数据丢失 |

> RAID0 无数据冗余，适合临时测试数据。生产环境请考虑 RAID1/RAID5 或使用 NVMe-oF 远端存储。

## 持久化配置

脚本执行 `setup` 后会自动写入以下配置实现重启后自动恢复：

- **`/etc/mdadm/mdadm.conf`**：RAID 阵列配置，用于重启后自动组装阵列
- **`/etc/fstab`**：自动挂载条目，格式为 `${RAID_DEV} ${MOUNT_POINT} xfs defaults,noatime 0 0`

> 如果 cleanup 后重新使用不同设备组建阵列，需要手动更新或删除旧的 fstab 条目。

## 常见问题

### mdadm 创建 RAID 失败

设备上可能存在旧的 RAID 元数据：

```bash
# 查看设备上的 RAID 元数据
mdadm --examine /dev/nvme4n1

# 手动擦除元数据
mdadm --zero-superblock /dev/nvme4n1

# 然后重新运行 setup
sudo ./scripts/tencent_env/setup_nvme_raid0.sh --devices "nvme4n1"
```

### 设备繁忙无法创建 RAID

可能有进程正在使用该设备：

```bash
# 查看哪个进程在使用设备
lsof | grep nvme4n1

# 或检查是否已挂载
mount | grep nvme
```

### 重启后 RAID 阵列未自动组装

确认 mdadm 配置已保存：

```bash
# 检查配置
cat /etc/mdadm/mdadm.conf

# 手动更新配置
sudo mdadm --detail --scan | sudo tee /etc/mdadm/mdadm.conf

# 手动组装
sudo mdadm --assemble --scan
```

### XFS 格式化失败

确认 `xfsprogs` 已安装：

```bash
sudo apt-get install xfsprogs
```

### 单盘不需要 RAID

如果只需使用一块 NVMe 设备且不需要 RAID，可以直接格式化挂载：

```bash
# 直接格式化
sudo mkfs.xfs -f /dev/nvme0n1

# 挂载
sudo mkdir -p /mnt/nvme0
sudo mount -o noatime /dev/nvme0n1 /mnt/nvme0
```
