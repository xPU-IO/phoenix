# Phoenix 从源码编译指南

本文档介绍如何使用 `scripts/tencent_env/build_phoenix.sh` 脚本从源码编译 Phoenix。

## 快速开始

```bash
# 全量编译（默认 Release 模式）
./scripts/tencent_env/build_phoenix.sh
```

编译成功后，产物位于项目根目录的 `build/` 下：

| 产物 | 路径 | 说明 |
|------|------|------|
| 内核模块 | `build/module/phoenixfs.ko` | Phoenix 内核模块 |
| 动态库 | `build/libphoenix.so` | 用户态共享库 |
| 静态库 | `build/libphoenix.a` | 用户态静态库（如未启用共享库） |
| 基准测试 | `build/bin/` | 各 benchmark 可执行文件 |

## 命令格式

```bash
./scripts/tencent_env/build_phoenix.sh [OPTIONS] [COMMAND]
```

### 命令（COMMAND）

| 命令 | 说明 |
|------|------|
| `build` | **默认**。完整编译：CMake 配置 + 全部编译 |
| `configure` | 仅运行 CMake 配置，不编译 |
| `module` | 仅编译内核模块（`phoenixfs.ko`） |
| `library` | 仅编译用户态库（`libphoenix`） |
| `clean` | 清理构建目录 |

### 选项（OPTIONS）

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `-b, --build-dir <dir>` | `./build` | 构建输出目录 |
| `-t, --type <type>` | `Release` | 构建类型：`Release`（优化）或 `Debug`（调试符号 + `-DDEBUG`） |
| `-a, --arch <archs>` | `90` | NVIDIA GPU 计算架构，分号分隔（如 `"80;90"`） |
| `-j, --jobs <N>` | `nproc` | 并行编译任务数 |
| `--no-module` | - | 跳过内核模块编译 |
| `--no-cuda` | - | 跳过 CUDA 支持（仅编译无 GPU 的库版本） |
| `-c, --clean` | - | 编译前先清理构建目录 |
| `-h, --help` | - | 显示帮助信息 |

## 使用示例

### 基础用法

```bash
# 默认全量编译（Release，所有 CPU 核心并行）
./scripts/tencent_env/build_phoenix.sh
```

### 开发调试

```bash
# Debug 模式 + 清理重建
./scripts/tencent_env/build_phoenix.sh --type Debug --clean
```

### 指定 GPU 架构

Phoenix 使用 CMake 的 `nvidia_archs` 变量控制 NVCC 目标架构，默认为 `90`（Hopper）。

```bash
# 同时支持 Ampere (80) 和 Hopper (90)
./scripts/tencent_env/build_phoenix.sh --arch "80;90"

# 仅支持 Hopper
./scripts/tencent_env/build_phoenix.sh --arch "90"
```

> 常见 NVIDIA 计算架构值：`70`（Volta）、`75`（Turing）、`80`（Ampere）、`86`（Ampere）、`90`（Hopper）。

### 部分编译

```bash
# 仅编译内核模块（适合调试内核模块时快速迭代）
./scripts/tencent_env/build_phoenix.sh module

# 仅编译用户态库
./scripts/tencent_env/build_phoenix.sh library

# 跳过内核模块（无需 root 权限、无需 kernel headers 的场景）
./scripts/tencent_env/build_phoenix.sh --no-module

# 不编译 CUDA（仅内核模块，无 GPU 库）
./scripts/tencent_env/build_phoenix.sh --no-cuda module
```

### 自定义构建目录与并行度

```bash
# 指定构建目录
./scripts/tencent_env/build_phoenix.sh --build-dir /tmp/phoenix-build

# 限制并行度（适合内存受限的环境）
./scripts/tencent_env/build_phoenix.sh --jobs 4
```

### 清理

```bash
# 清理构建目录
./scripts/tencent_env/build_phoenix.sh clean

# 清理后重新编译
./scripts/tencent_env/build_phoenix.sh --clean
```

## 编译流程详解

脚本按以下顺序执行：

1. **依赖检测**：自动检查 cmake、gcc/g++、CUDA、kernel headers、liburing 等是否可用
2. **CMake 配置**：生成构建文件，检测 NVIDIA 驱动头文件和 `Module.symvers`
3. **内核模块编译**：通过 Kbuild 系统编译 `phoenixfs.ko`（含 `module/configure` 特性检测）
4. **用户态库编译**：编译 `libphoenix`（CUDA + io_uring）
5. **Benchmark 编译**：编译所有基准测试可执行文件
6. **构建摘要**：列出所有产物路径和下一步操作建议

> 若构建目录中已存在 `CMakeCache.txt`，跳过配置步骤直接编译；如需重新配置，使用 `--clean`。

## 依赖检测详情

脚本在 `build`、`configure`、`module`、`library` 命令执行前自动检测依赖：

| 检测项 | 必需条件 | 失败行为 |
|--------|----------|----------|
| CMake | >= 3.18 | 报错退出 |
| GCC | 可执行 | 报错退出 |
| G++ | 可执行 | 报错退出 |
| Kernel Headers | `/lib/modules/$(uname -r)/build` 存在 | 报错退出（`--no-module` 时跳过） |
| CUDA (nvcc) | >= 12.4 | 报错退出（`--no-cuda` 时跳过） |

## 编译后的下一步，加载内核模块

编译完成后，脚本会输出下一步操作提示：

```bash
# 加载内核模块
sudo ./scripts/tencent_env/setup_phoenix_module.sh

