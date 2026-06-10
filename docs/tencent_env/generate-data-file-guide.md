# Phoenix KVCache 测试数据生成指南

本文档介绍如何使用 `scripts/tencent_env/generate_data_file.sh` 脚本生成用于 kvcache benchmark 的确定性测试数据文件。

## 前提条件

- 需要 **root 权限**（`sudo`），以确保对 NVMe 挂载点的写入权限
- Python 3 已安装（脚本内部使用 Python 生成数据）

## 数据文件格式

脚本生成的数据文件采用**确定性模式**（deterministic pattern）：

- 文件中每 8 字节存储其自身的文件偏移量（little-endian `uint64`）
- 即偏移量 `0x0000` 处存储 `0x0000000000000000`，偏移量 `0x0008` 处存储 `0x0000000000000008`，依此类推

这种设计的优势：

| 用途 | 说明 |
|------|------|
| **IO 正确性验证** | 读取后直接比对数据是否等于其偏移量，即可判断 IO 是否正确 |
| **无额外元数据** | 数据完全自描述，无需维护独立的校验文件 |
| **可验证任意位置** | 支持全量验证或随机采样验证 |

配合 `verify_io_correctness.sh` 脚本可对生成的文件进行 IO 正确性校验。

## 快速开始

```bash
# 生成 20GB 默认数据文件（/mnt/nvme4/kvcache_tensor.bin）
sudo ./scripts/tencent_env/generate_data_file.sh
```

## 命令格式

```bash
sudo ./scripts/tencent_env/generate_data_file.sh [OPTIONS]
```

### 选项（OPTIONS）

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `-m, --mount <dir>` | `/mnt/nvme4` | NVMe 挂载点目录（用于推断默认输出路径） |
| `-s, --size <GB>` | `20` | 数据文件大小（GB，正整数） |
| `-o, --output <path>` | `${MOUNT_POINT}/kvcache_tensor.bin` | 输出文件路径（指定后忽略 `--mount` 的路径推断） |
| `-h, --help` | - | 显示帮助信息 |

> `--mount` 和 `--output` 的关系：若指定了 `--output`，则输出路径以 `--output` 为准；否则输出到 `${MOUNT_POINT}/kvcache_tensor.bin`。

## 使用示例

### 默认生成

```bash
# 20GB，输出到 /mnt/nvme4/kvcache_tensor.bin
sudo ./scripts/tencent_env/generate_data_file.sh
```

### 自定义大小

```bash
# 生成 50GB 数据文件
sudo ./scripts/tencent_env/generate_data_file.sh --size 50

# 生成 100GB 数据文件
sudo ./scripts/tencent_env/generate_data_file.sh --size 100
```

### 自定义输出路径

```bash
# 指定完整输出路径
sudo ./scripts/tencent_env/generate_data_file.sh --output /data/test_tensor.bin --size 10

# 使用不同的挂载点
sudo ./scripts/tencent_env/generate_data_file.sh --mount /mnt/nvme0 --size 30
```

### 重新生成

```bash
# 如已有文件大小或模式不匹配，脚本会自动重新生成
# 如需强制重新生成，先删除旧文件
sudo rm /mnt/nvme4/kvcache_tensor.bin
sudo ./scripts/tencent_env/generate_data_file.sh
```

## 生成流程详解

1. **参数验证**：确认 `--size` 为正整数
2. **已有文件检查**：
   - 若文件已存在且大小匹配，进行快速模式校验（检查首 8 字节是否为 `0`，末 8 字节是否为 `文件大小 - 8`）
   - 大小和模式均匹配 → 跳过生成，直接返回
   - 大小不匹配或模式校验失败 → 重新生成
3. **数据生成**（Python）：
   - 以 256MB 为单位分块写入
   - 每个分块内，每 8 字节填充其全局文件偏移量
   - 实时显示进度百分比
4. **完成提示**

### 进度输出示例

```
=== Generating 20GB deterministic data file ===
Pattern: each 8-byte position stores its file offset (little-endian uint64)
  Progress: 100% (20480MB / 20480MB)
Data file generated: /mnt/nvme4/kvcache_tensor.bin
```

## 已有文件跳过机制

脚本不会盲目覆盖已有文件，而是智能判断是否需要重新生成：

| 文件状态 | 脚本行为 |
|----------|----------|
| 不存在 | 生成新文件 |
| 存在，大小匹配，首尾模式正确 | 跳过，提示 "already exists with correct size and deterministic pattern" |
| 存在，大小不匹配 | 提示 size mismatch，重新生成 |
| 存在，大小匹配但模式错误 | 提示 pattern mismatch，重新生成 |

> 模式校验采用快速策略：仅检查首 8 字节（应为 `0`）和末 8 字节（应为 `文件大小 - 8`），不遍历全部数据。完整验证请使用 `verify_io_correctness.sh`。

## IO 正确性验证

数据文件生成后，可使用 `verify_io_correctness.sh` 进行完整性校验：

```bash
# 全量验证
sudo ./scripts/tencent_env/verify_io_correctness.sh

# 随机采样 1% 验证（适合大文件快速校验）
sudo ./scripts/tencent_env/verify_io_correctness.sh --sample 0.01

# 验证前 1000 个块
sudo ./scripts/tencent_env/verify_io_correctness.sh --count 1000
```

## 性能参考

数据生成速度主要受 NVMe 写入带宽限制：

| 配置 | 20GB 生成时间（参考） |
|------|----------------------|
| 单块 NVMe | ~10-15 秒 |
| 4 块 NVMe RAID0 | ~3-5 秒 |

> 实际时间取决于 NVMe 设备性能和 CPU 单核速度（Python 生成为单线程）。