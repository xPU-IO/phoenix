# Phoenix 内核模块加载/卸载指南

本文档介绍如何使用 `scripts/tencent_env/setup_phoenix_module.sh` 脚本管理 Phoenix 内核模块（`phoenixfs.ko`）的加载与卸载。

## 前提条件

- 已编译 Phoenix 内核模块（`phoenixfs.ko` 位于 `build/module/` 下），参见 [编译指南](build_phoenix-guide.md)
- 模块已经完成签名
- 需要 **root 权限**（`sudo`）
- NVIDIA 驱动已初始化（`nvidia-smi` 可正常运行）

## 快速开始

```bash
# 加载模块（默认对所有 NUMA 节点生效）
sudo ./scripts/tencent_env/setup_phoenix_module.sh

# 卸载模块
sudo ./scripts/tencent_env/setup_phoenix_module.sh unload
```

## 命令格式

```bash
sudo ./scripts/tencent_env/setup_phoenix_module.sh [OPTIONS] [COMMAND]
```

### 命令（COMMAND）

| 命令 | 说明 |
|------|------|
| `load` | **默认**。加载 Phoenix 内核模块 |
| `unload` | 卸载 Phoenix 内核模块 |

### 选项（OPTIONS）

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `-n, --numa <node>` | `all`（即 `-1`） | NUMA 节点过滤。`-1` 或不指定表示对所有 NUMA 节点生效；指定具体数字（如 `0`、`1`）则仅对该节点生效 |
| `-d, --module-dir <dir>` | `build/module` | `phoenixfs.ko` 所在目录 |
| `-f, --force` | - | 强制卸载（模块被占用时使用 `modprobe -r` 尝试卸载） |
| `-h, --help` | - | 显示帮助信息 |

## NUMA 节点过滤说明

Phoenix 内核模块通过 `phxfs_numa_node` 参数控制 GPU 设备的 NUMA 过滤行为：

| `phxfs_numa_node` 值 | 行为 |
|----------------------|------|
| `-1`（默认） | 不过滤，对所有 NUMA 节点上的 GPU 生效 |
| `0` | 仅对 NUMA 节点 0 上的 GPU 生效 |
| `1` | 仅对 NUMA 节点 1 上的 GPU 生效 |
| 其他正整数 | 仅对指定 NUMA 节点上的 GPU 生效 |

> 该参数对应内核模块源码 `module/phxfs.c` 中的 `phxfs_numa_node` module_param，当值 >= 0 时会跳过不属于该 NUMA 节点的 GPU 设备。

### 查看系统 NUMA 拓扑

```bash
# 查看 NUMA 节点与 GPU 的对应关系
nvidia-smi topo -m

# 查看 NUMA 节点信息
numactl --hardware
```

## 使用示例

### 加载模块

```bash
# 默认加载：对所有 NUMA 节点生效
sudo ./scripts/tencent_env/setup_phoenix_module.sh

# 仅对 NUMA 节点 0 上的 GPU 生效
sudo ./scripts/tencent_env/setup_phoenix_module.sh --numa 0

# 指定模块路径
sudo ./scripts/tencent_env/setup_phoenix_module.sh --module-dir /path/to/module
```

### 卸载模块

```bash
# 正常卸载
sudo ./scripts/tencent_env/setup_phoenix_module.sh unload

# 强制卸载（模块被占用时）
sudo ./scripts/tencent_env/setup_phoenix_module.sh -f unload
```

### 重复加载

如果模块已加载，脚本会检测到并显示当前参数，不会重复执行 `insmod`：

```bash
$ sudo ./scripts/tencent_env/setup_phoenix_module.sh
Phoenix module already loaded
Current phxfs_numa_node=all (expected: all)
```

> 如需修改 NUMA 参数，需先卸载再重新加载。

## 加载流程详解

脚本执行 `load` 命令时按以下步骤进行：

1. **Root 权限检查**：非 root 用户直接报错退出
2. **重复加载检测**：通过 `lsmod` 检查模块是否已加载，已加载则显示当前参数并退出
3. **NVIDIA 驱动初始化验证**：执行 `nvidia-smi` 确保驱动已加载
4. **模块文件检查**：确认 `phoenixfs.ko` 存在于指定目录
5. **执行 insmod**：加载模块并传入 `phxfs_numa_node` 参数
6. **加载结果验证**：
   - 通过 `lsmod` 确认模块出现在内核模块列表中
   - 通过 `/sys/module/phoenixfs/parameters/phxfs_numa_node` 验证参数是否正确传入
   - 显示 `dmesg` 中 `phxfs:` 开头的内核日志

## 卸载流程详解

脚本执行 `unload` 命令时按以下步骤进行：

1. **Root 权限检查**
2. **加载状态检测**：模块未加载则直接返回
3. **尝试 rmmod**：正常卸载模块
4. **卸载失败处理**：
   - **模块被占用**（refcount > 0）：提示占用该模块的依赖模块列表，建议停止相关进程
   - **加 `--force` 时**：尝试 `modprobe -r` 强制卸载依赖链
   - **其他错误**：显示 `dmesg` 相关日志辅助排查

### 模块被占用时的输出示例

```bash
$ sudo ./scripts/tencent_env/setup_phoenix_module.sh unload
=== Unloading Phoenix module ===
Warning: Phoenix module is currently in use (refcount=1)
The following processes may be using it:
  - nvidia_fs

Please stop the processes above before unloading, or use -f/--force to force unload.
```

此时可以：

```bash
# 方式一：先卸载依赖模块，再卸载 Phoenix
sudo rmmod nvidia_fs
sudo ./scripts/tencent_env/setup_phoenix_module.sh unload

# 方式二：使用 --force 强制卸载
sudo ./scripts/tencent_env/setup_phoenix_module.sh -f unload
```

> **注意**：强制卸载可能导致使用该模块的进程崩溃，请确保相关进程已安全停止。

## 运行时参数查看与修改

模块加载后，可通过 sysfs 查看 `phxfs_numa_node` 参数：

```bash
# 查看当前值
cat /sys/module/phoenixfs/parameters/phxfs_numa_node
