# Phoenix IO 正确性验证指南

本文档介绍如何使用 `scripts/tencent_env/verify_io_correctness.sh` 脚本验证 kvcache 测试数据文件的 IO 正确性。

## 前提条件

- 已生成确定性数据文件（参见 [数据文件生成指南](generate-data-file-guide.md)）
- Python 3 已安装（脚本内部使用 Python 执行验证逻辑）
- `bc` 命令可用（采样模式下计算比例）


## 验证原理

数据文件由 `generate_data_file.sh` 生成，采用确定性模式：**每 8 字节存储其自身的文件偏移量**（little-endian `uint64`）。

验证脚本读取文件数据，批量检查每个 8 字节位置的实际值是否等于预期的文件偏移量。脚本会自动检测 numpy 是否可用，优先使用 numpy 向量化比较（最快），否则使用批量 `struct.unpack`（次快）：

```
偏移量 0x0000:  预期值 0x0000000000000000  →  读取值 0x0000000000000000  ✓
偏移量 0x0008:  预期值 0x0000000000000008  →  读取值 0x0000000000000008  ✓
偏移量 0x0010:  预期值 0x0000000000000010  →  读取值 0x0000000000000010  ✓
...
```

任何不匹配即表示 IO 错误（数据损坏、静默数据损坏等）。

## 快速开始

```bash
# 全量验证默认数据文件
sudo ./scripts/tencent_env/verify_io_correctness.sh
```

## 命令格式

```bash
sudo ./scripts/tencent_env/verify_io_correctness.sh [OPTIONS]
```

### 选项（OPTIONS）

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `-f, --file <path>` | `/mnt/nvme4/kvcache_tensor.bin` | 待验证的数据文件路径 |
| `-b, --block-size <B>` | `16384`（16KB） | 块大小（字节），必须为 8 的正整数倍 |
| `-c, --count <N>` | `0`（全部） | 验证前 N 个块，`0` 表示验证全部 |
| `-s, --sample <ratio>` | `0`（不采样） | 随机采样比例，如 `0.01` 表示 1% |
| `--seed <N>` | `42` | 采样模式下的随机种子 |
| `-h, --help` | - | 显示帮助信息 |

## 验证模式

脚本支持三种验证模式，按以下优先级自动选择：

### 1. 采样模式（`--sample` > 0）

随机抽取指定比例的块进行验证，适合大文件快速校验。

```bash
# 随机采样 1% 的块
sudo ./scripts/tencent_env/verify_io_correctness.sh --sample 0.01

# 采样 5%，使用自定义种子确保可重复
sudo ./scripts/tencent_env/verify_io_correctness.sh --sample 0.05 --seed 123
```

| 参数 | 说明 |
|------|------|
| 采样数量 | `总块数 × 采样比例`，至少 1 块 |
| 与 `--count` 组合 | 取 `采样数量` 和 `count` 的较小值 |
| 随机性 | 使用 `--seed` 控制种子，相同种子相同采样结果 |

### 2. 计数模式（`--count` > 0 且 `--sample` = 0）

验证从文件开头起的前 N 个块。

```bash
# 验证前 100 个块
sudo ./scripts/tencent_env/verify_io_correctness.sh --count 100

# 验证前 1000 个块，块大小 64KB
sudo ./scripts/tencent_env/verify_io_correctness.sh --count 1000 --block-size 65536
```

### 3. 全量模式（默认）

验证文件中所有块，最彻底但耗时最长。

```bash
# 全量验证
sudo ./scripts/tencent_env/verify_io_correctness.sh
```

## 使用示例

### 基本验证

```bash
# 全量验证默认数据文件
sudo ./scripts/tencent_env/verify_io_correctness.sh
```

### 自定义文件

```bash
# 验证指定路径的数据文件
sudo ./scripts/tencent_env/verify_io_correctness.sh --file /data/test_tensor.bin
```

### 快速采样验证

```bash
# 1% 采样，适合快速确认文件整体正确性
sudo ./scripts/tencent_env/verify_io_correctness.sh --sample 0.01

# 0.1% 采样，极快
sudo ./scripts/tencent_env/verify_io_correctness.sh --sample 0.001
```

### 自定义块大小

```bash
# 64KB 块大小（匹配特定 benchmark 配置）
sudo ./scripts/tencent_env/verify_io_correctness.sh --block-size 65536

# 4KB 块大小（更细粒度检测）
sudo ./scripts/tencent_env/verify_io_correctness.sh --block-size 4096
```

> 块大小影响验证粒度和速度：较小的块能更精确定位错误位置，但验证速度略慢。默认 16KB 与 kvcache benchmark 的配置一致。

### 组合使用

```bash
# 自定义文件 + 采样 + 种子
sudo ./scripts/tencent_env/verify_io_correctness.sh \
  --file /data/test.bin \
  --sample 0.05 \
  --seed 123 \
  --block-size 65536
```

## 输出说明

### 验证通过

```
=== Full Verification ===
Verifying all 1310720 blocks
File: /mnt/nvme4/kvcache_tensor.bin
File size: 21474836480 bytes (20480MB)
Block size: 16384 bytes
Blocks to verify: 1310720

  Progress: 100% (1310720/1310720 blocks, 1200.5 MB/s, numpy)

=== Verification Result ===
PASSED: All 1310720 blocks verified successfully
  Bytes verified: 21474836480 (20480.0 MB)
  Throughput: 1200.5 MB/s
  Acceleration: numpy
  Time: 17.05s
```

### 验证失败

```
=== Full Verification ===
Verifying all 1310720 blocks
File: /mnt/nvme4/kvcache_tensor.bin
File size: 21474836480 bytes (20480MB)
Block size: 16384 bytes
Blocks to verify: 1310720

  Block 42 (offset 0x2a000): expected 172032, got 0

=== Verification Result ===
FAILED: 20 error(s) found (showing first 20)
  Offset 0x2a000 (block 42): expected 172032, got 0
  Offset 0x2a008 (block 42): expected 172040, got 0
  Offset 0x2a010 (block 42): expected 172048, got 0
  ...
  Error blocks: 1
  First error at offset: 0x2a000
```

| 输出字段 | 说明 |
|----------|------|
| `Progress` | 验证进度百分比、已验证块数、吞吐量、加速方式 |
| `PASSED / FAILED` | 验证结果 |
| `Bytes verified` | 已验证的总字节数 |
| `Throughput` | 验证吞吐量（MB/s） |
| `Acceleration` | 使用的加速方式：`numpy`（最快）或 `batch-struct`（次快） |
| `Time` | 总耗时 |
| `Offset 0x...` | 错误位置的详细偏移量、预期值和实际值（最多显示 20 条） |
| `Error blocks` | 包含错误的块数 |
| `First error at offset` | 首个错误位置（十六进制），便于定位 |

> 脚本退出码：`0` = 验证通过，`1` = 验证失败。

## 验证流程详解

1. **参数验证**：检查文件存在性、块大小为 8 的正整数倍、count 为非负整数
2. **文件信息计算**：统计文件大小、总块数
3. **模式选择**：按 `--sample` > `--count` > 全量 的优先级确定验证模式
4. **加速检测**：自动检测 numpy 是否可用，优先使用 numpy 向量化比较
5. **批量验证**（Python）：
   - **全量/计数模式**：顺序读取（4MB buffer），大幅减少系统调用和 seek 开销
   - **采样模式**：按块 seek + read（保持随机访问特性）
   - 批量 `struct.unpack` 或 numpy 向量化比较：一次性解包整个读取块，避免逐 8 字节调用
   - 记录错误详情（最多 20 条）和错误块数
   - 每 10000 块或最后一块时刷新进度
6. **结果输出**：显示通过/失败、吞吐量、加速方式、耗时、错误详情

## 块大小选择建议

| 场景 | 推荐块大小 | 说明 |
|------|-----------|------|
| kvcache benchmark | 16KB（默认） | 与 `kvcache.sh` 的 block size 一致 |
| 通用 IO 验证 | 16KB-64KB | 平衡粒度和速度 |
| 精确定位错误 | 4KB | 更细粒度，但速度较慢 |
| 快速扫描 | 1MB | 速度快，但错误定位精度低 |

> 块大小必须是 8 的正整数倍（因为验证单位是 8 字节的 `uint64`）。


## 常见问题

### 文件未找到

```
Error: Data file not found: /mnt/nvme4/kvcache_tensor.bin
```

需先生成数据文件：

```bash
sudo ./scripts/tencent_env/generate_data_file.sh
```

### 块大小不是 8 的倍数

```
Error: --block-size must be a positive integer multiple of 8
```

块大小必须是 8 的正整数倍（如 4096、8192、16384、65536 等）。

### bc 命令未找到（采样模式）

```
$(echo "${SAMPLE_RATIO} > 0" | bc -l): command not found
```

安装 `bc`：

```bash
sudo yum install bc
```

### 验证失败但不确定原因

1. 查看错误偏移量和实际值，判断是全零（IO 未写入）、偏移（数据错位）还是随机值（数据损坏）
2. 使用更小的块大小重新验证，精确定位错误范围
3. 检查 NVMe 设备健康状态：`smartctl -a /dev/nvme0n1`
4. 检查 Phoenix 内核模块状态：`lsmod | grep phoenixfs`

### 验证速度过慢

脚本已内置批量解包和顺序读取优化。如果速度仍然不理想：

- 安装 numpy 获得向量化加速：`pip3 install numpy`
- 使用采样模式快速验证：`--sample 0.01`
- 使用计数模式验证前 N 块：`--count 1000`
- 检查输出中的 `Acceleration` 字段，确认是否使用了 `numpy` 加速
- 检查 NVMe 设备读取带宽是否正常：`hdparm -Tt /dev/nvme0n1`
