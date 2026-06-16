# 优化 2：phxfs_regmem / 本地 P2P mmap 去除 “2GB/次” 限制 — TODO

> 目标：搞清 “单次注册超过 2GB 就要多次 mmap” 的真正根因，并从根上去掉这条限制，
> 进而简化用户态的多段 mmap / 链表 / xfer 切块逻辑。

## 背景与待验证的分歧点

`phxfs_regmem` 实际分两步：
1. **第一步：mmap**（自定义 mmap，后端 = phoenix 内核模块 `phxfs_mmap`），返回一段“空指针”VMA。
2. **第二步：ioctl(PHXFS_IOCTL_MAP)**，内核 `phxfs_map_dev_addr_inner` 里 `nvidia_p2p_get_pages` +
   `vm_insert_page` 把 GPU P2P 页插进第一步的 VMA。

- 我的静态分析：当前源码 `phxfs_add_phony_buffer` 在 mmap 步只 `kzalloc` 一个**固定大小**结构体，
  不随映射长度增长 → 理论上 mmap 步不该卡 2GB；卡点应在第二步 `ioctl` 的
  `kmalloc(ppages = host_page_num * 8)`，撞 `KMALLOC_MAX_SIZE`（4MiB），对应 region ≤ 2GiB。
- 用户记忆：**第一步 mmap 本身**就分配不了 >2GB 的空指针。
- ⇒ **先用测试程序实测，确定卡点在 mmap 步还是 ioctl 步，以及精确的边界 size。**

## TODO

- [ ] **T1 实测定位**：写 `tests/test_regmem_limit.cu`，两段扫描：
      - Phase 1：纯 `mmap(/dev/phxfs_devN)` 扫不同 size（不做 ioctl）→ 看 mmap 步是否卡 2GB。
      - Phase 2：单次 `mmap` + 单次 `ioctl(PHXFS_IOCTL_MAP)`（真实 cudaMalloc 指针）→ 看 ioctl 步边界。
      - 记录每个 size 的成功/失败 + errno，定位精确边界（预期 ppages=size/512 撞 KMALLOC_MAX_SIZE）。
      - 资源：GPU 0 / GPU 1；文件 /mnt/raid1；模块已装；设备 0 = BDF 06:00.0。
- [x] **T2 内核改 kvmalloc**（直击根因）：`module/phxfs-mem.c` 已完成。
      `ppages`(kvmalloc_array)、`dev_page_addrs`(kvcalloc) + 对应 kvfree；`map` 暂留 kmalloc(≤32GiB)。
      失败路径健壮化：ppages 判空、get_pages ret/gd->pages 判空、out: 先 put_pages 再释放(消除 UAF)。
      **实测验证 (GPU0)**：Phase 2 单次 mmap+ioctl 0.5G~4G 全 ret=0 OK，ppages 到 8MiB，无 panic。
- [x] **T3 mmap 步是否受限**：已实测排除。Phase 1 纯 mmap 一路到 16GiB 全 OK(errno=0)，
      证明 `phxfs_mmap`/`phxfs_add_phony_buffer` 是 lazy 的，不卡 2GB。限制在第二步 ioctl。
- [x] **T1 实测定位（部分）**：Phase 1 已确认 mmap 步无限制。Phase 2 待内核修复后再全扫描。
      另：Phase 2 在未修复模块上 >2GB 会 panic（失败路径空指针解引用 + use-after-free，
      `nvfs_nvidia_p2p_get_pages` ret 未检查、`ppages` 未判空、`out:` 在 pin 成功后 kfree(map/gd) 造成 UAF）。
- [x] **T4 用户态简化**：`libphoenix/phoenix.cc` `phxfs_regmem` 已改为单次 `mmap(len)` + 单次 `ioctl`，
      节点结构 `vaddrs[]`→单 `vaddr`，删 `mmap_count`/`length_left`；`phxfs_deregmem` 同步单段化；
      `MMAP_LIMIT` 改名 `PHXFS_IO_CHUNK`(1GiB) 仅用于 I/O syscall 分段。（待实测）
- [x] **T5 read/write 简化**：`phxfs_do_xfer_addr` 改为连续映射(vaddr+offset)，仅按 `PHXFS_IO_CHUNK`
      切分以规避单次 syscall 的 `MAX_RW_COUNT(~2GiB)`；`phxfs_read/write` 循环结构不变直接复用。（待实测）
- [x] **T6 xfer 简化**：已随 T5 完成（contiguous 版本，async 路径 integration.cc 复用同一函数）。
- [ ] **T7 回归 + 正确性**：`tests/test_regmem_io.cu`（单次注册 >2GB + phxfs_read + 8字节偏移量校验）；
      example / end-to-end / verify_io_correctness。
      **实测通过 (GPU0)**：单次注册 3 / 4 / 16 GiB，phxfs_read 走 P2P 读入 GPU 后逐 8 字节校验全 PASSED。
      [ ] phxfs_write 路径回归（可选，未做）。
      [ ] example/end-to-end 回归（可选，未跑）。

## 已知遗留 / 后续

- 单次注册 **>32GiB** 仍会失败：`map` 描述符仍用 kmalloc（≈region/8192，32GiB 时撞 4MiB）。
  已是优雅失败(返回 -ENOMEM，不崩)。若需 >32GiB 单次注册，把 `map` 也改 kvmalloc，
  并同步把 `release_gpu_memory`/`force_release_gpu_memory`/`unmap_and_release` 的 `kfree(map)` 改 kvfree
  （注意 force-release 是 nvidia 驱动回调上下文，确认可 sleep 再用 kvfree）。
- 内核 5.4.241-tlinux4，MAX_ORDER=11 → KMALLOC_MAX_SIZE=4MiB，印证 2GiB 边界推导。

## 关键数字备忘

- GPU 页 64KiB，host 页 4KiB，subpage_num=16；host_page_num = region / 4KiB。
- `ppages` 字节数 = host_page_num × 8 = **region / 512**。
- region 2GiB ⇒ ppages = 4MiB（= 老内核 MAX_ORDER=11 的 KMALLOC_MAX_SIZE）。region >2GiB ⇒ kmalloc 失败。
- 注意独立的坎：单次 read()/write()/io_uring 最多传 `MAX_RW_COUNT ≈ 2GiB`。

## 关联：优化 1（io_uring 异步）— 后续单列

- `integration.cc` 现状是 `cudaLaunchHostFunc + pread/pwrite`，非 io_uring，且 data/addrs 泄漏。
- 计划用 io_uring 重写 `phxfs_read_async/write_async`；腾讯内核疑点：io_uring 取页路径对
  ZONE_DEVICE / PCI_P2PDMA 页的 GUP 兼容性。待优化 2 落地后开。
