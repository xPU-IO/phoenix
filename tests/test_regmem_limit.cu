// test_regmem_limit.cu
//
// 目的：实测 phxfs 本地 P2P 注册的 "2GB/次" 限制到底卡在哪一步。
//   Phase 1: 纯 mmap(/dev/phxfs_devN)，不做 ioctl —— 验证 "第一步 mmap" 是否卡 2GB。(安全)
//   Phase 2: 单次 mmap + 单次 ioctl(PHXFS_IOCTL_MAP)（真实 cudaMalloc 指针）
//            —— 验证 "第二步 insert" 的边界（预期 ppages = size/512 撞 KMALLOC_MAX_SIZE 4MiB => 2GiB）。
//
// 设备节点 crw-rw-rw-，无需 sudo。
//
// 编译：
//   nvcc -O2 -std=c++17 tests/test_regmem_limit.cu -o /tmp/test_regmem_limit -lcudart
//
// 运行：
//   /tmp/test_regmem_limit [cuda_gpu_id]                 # 默认只跑 Phase 1（纯 mmap，安全）
//   /tmp/test_regmem_limit [cuda_gpu_id] --phase2        # 跑到 Phase 2，默认封顶 2GiB
//   /tmp/test_regmem_limit [cuda_gpu_id] --phase2 --all  # 危险：扫到 4GiB
//
// ⚠️ 警告：在【未修复】的 phxfs 内核模块上，Phase 2 跑到 >2GiB 会触发模块失败路径里的
//    空指针解引用 / use-after-free，可能直接 panic 整机。务必先修内核错误处理 + kvmalloc，
//    rebuild + reload 之后再用 --all。

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <fstream>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <asm/ioctl.h>

#include <cuda_runtime.h>

// ---- 复刻 module/include 的 ioctl ABI（避免引内核味头文件）----
struct phxfs_dev_info_s {
    __u64 dev_id;
} __attribute__((packed, aligned(8)));

struct phxfs_ioctl_map_s {
    struct phxfs_dev_info_s dev;
    __u64 c_vaddr;
    __u64 c_size;
    __u64 n_vaddr;
    __u64 n_size;
    __u64 end_addr;
    __u32 sbuf_block;
} __attribute__((packed, aligned(8)));

#define PHXFS_IOCTL        0x88
#define PHXFS_IOCTL_MAP    _IOW(PHXFS_IOCTL, 1, struct phxfs_ioctl_map_s)
#define PHXFS_IOCTL_UNMAP  _IOW(PHXFS_IOCTL, 2, struct phxfs_ioctl_map_s)

#define GIB (1024ULL * 1024 * 1024)
#define MIB (1024ULL * 1024)
#define HUGE_PAGE (64ULL * 1024)

static inline double to_gib(uint64_t b) { return (double)b / (double)GIB; }

// 找到 cuda gpu 对应的 phxfs 设备号（复刻 libphoenix 逻辑：比对 BDF）
static int find_phxfs_dev_for_cuda_gpu(int cuda_gpu_id) {
    int dom = 0, bus = 0, dev = 0;
    if (cudaDeviceGetAttribute(&dom, cudaDevAttrPciDomainId, cuda_gpu_id) != cudaSuccess) return -1;
    if (cudaDeviceGetAttribute(&bus, cudaDevAttrPciBusId,   cuda_gpu_id) != cudaSuccess) return -1;
    if (cudaDeviceGetAttribute(&dev, cudaDevAttrPciDeviceId, cuda_gpu_id) != cudaSuccess) return -1;
    for (int i = 0; i < 8; i++) {
        std::string p = "/sys/class/phxfs-generic/phxfs_dev" + std::to_string(i) + "/pci_bdf";
        std::ifstream ifs(p);
        if (!ifs.is_open()) break;
        unsigned int d, b, dd, fn; char s1, s2, s3;
        ifs >> std::hex >> d >> s1 >> b >> s2 >> dd >> s3 >> fn;
        if (d == (unsigned)dom && b == (unsigned)bus && dd == (unsigned)dev)
            return i;
    }
    return -1;
}

// ---------- Phase 1: 纯 mmap 扫描（安全，无 ioctl）----------
static void phase1_raw_mmap(const char *devpath) {
    printf("\n===== Phase 1: raw mmap only (no ioctl) on %s =====\n", devpath);
    printf("%-14s %-8s %-18s %s\n", "size", "result", "addr", "errno");
    uint64_t sizes[] = {
        512 * MIB, 1 * GIB, (uint64_t)(1.5 * GIB), 2 * GIB,
        2 * GIB + HUGE_PAGE, 3 * GIB, 4 * GIB, 8 * GIB, 16 * GIB
    };
    for (uint64_t sz : sizes) {
        int fd = open(devpath, O_RDWR);
        if (fd < 0) { printf("%-11.3fGiB open fail errno=%d (%s)\n", to_gib(sz), errno, strerror(errno)); continue; }
        errno = 0;
        void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            printf("%-11.3fGiB %-8s %-18s errno=%d (%s)\n", to_gib(sz), "FAIL", "-", errno, strerror(errno));
        } else {
            printf("%-11.3fGiB %-8s %-18p (errno=%d)\n", to_gib(sz), "OK", p, errno);
            munmap(p, sz);
        }
        close(fd);
    }
}

// ---------- Phase 2: 单次 mmap + 单次 ioctl(MAP)（会触发内核 insert 路径）----------
static void phase2_single_shot_regmem(const char *devpath, int dev_id, int cuda_gpu_id, bool allow_over_2g) {
    printf("\n===== Phase 2: single mmap + single ioctl(MAP) (cuda gpu %d, %s) =====\n",
           cuda_gpu_id, allow_over_2g ? "UP TO 4GiB - DANGEROUS" : "capped at 2GiB");
    printf("%-14s %-12s %-8s %-8s %s\n", "size", "cudaMalloc", "mmap", "ioctl", "errno/note");
    std::vector<uint64_t> sizes = { 512 * MIB, 1 * GIB, (uint64_t)(1.5 * GIB), 2 * GIB };
    if (allow_over_2g) {
        sizes.push_back(2 * GIB + HUGE_PAGE);
        sizes.push_back(3 * GIB);
        sizes.push_back(4 * GIB);
    }
    for (uint64_t sz : sizes) {
        void *gbuf = nullptr;
        cudaError_t cerr = cudaMalloc(&gbuf, sz);
        if (cerr != cudaSuccess) {
            printf("%-11.3fGiB %-12s -        -        cudaMalloc: %s\n",
                   to_gib(sz), "FAIL", cudaGetErrorString(cerr));
            continue;
        }
        // nvidia_p2p_get_pages 要求 GPU 基址 64KiB 对齐；cudaMalloc 大块通常已对齐，这里兜底
        uint64_t gpu = (uint64_t)gbuf;
        uint64_t gpu_aligned = (gpu + HUGE_PAGE - 1) & ~(HUGE_PAGE - 1);
        uint64_t reg_len = sz - (gpu_aligned - gpu);
        reg_len &= ~(HUGE_PAGE - 1);

        int fd = open(devpath, O_RDWR);
        if (fd < 0) { printf("%-11.3fGiB OK           open fail errno=%d\n", to_gib(sz), errno); cudaFree(gbuf); continue; }

        errno = 0;
        void *p = mmap(NULL, reg_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            printf("%-11.3fGiB %-12s %-8s %-8s errno=%d (%s)\n",
                   to_gib(sz), "OK", "FAIL", "-", errno, strerror(errno));
            close(fd); cudaFree(gbuf); continue;
        }

        struct phxfs_ioctl_map_s mp;
        memset(&mp, 0, sizeof(mp));
        mp.dev.dev_id = dev_id;
        mp.c_vaddr = (uint64_t)p;
        mp.c_size  = reg_len;
        mp.n_vaddr = gpu_aligned;
        mp.n_size  = reg_len;

        printf("%-11.3fGiB %-12s %-8s ", to_gib(sz), "OK", "OK");
        fflush(stdout);   // ioctl 可能让内核 panic，先把前缀刷出去

        errno = 0;
        int ret = ioctl(fd, PHXFS_IOCTL_MAP, &mp);
        printf("%-8s ret=%d errno=%d (%s) ppages=%.2fMiB\n",
               (ret == 0 ? "OK" : "FAIL"),
               ret, errno, strerror(errno), (double)(reg_len / 4096 * 8) / (double)MIB);

        if (ret == 0) {
            ioctl(fd, PHXFS_IOCTL_UNMAP, &mp);  // 成功则释放，避免泄漏 pin 住的 GPU 页
        }
        munmap(p, reg_len);
        close(fd);
        cudaFree(gbuf);
    }
}

int main(int argc, char **argv) {
    int cuda_gpu_id = 0;
    bool do_phase2 = false;
    bool allow_over_2g = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--phase2") do_phase2 = true;
        else if (a == "--all") allow_over_2g = true;
        else cuda_gpu_id = atoi(argv[i]);
    }

    if (cudaSetDevice(cuda_gpu_id) != cudaSuccess) {
        printf("cudaSetDevice(%d) failed\n", cuda_gpu_id);
        return 1;
    }
    int dev_id = find_phxfs_dev_for_cuda_gpu(cuda_gpu_id);
    if (dev_id < 0) {
        printf("WARN: no phxfs dev matched cuda gpu %d, fallback to phxfs_dev0\n", cuda_gpu_id);
        dev_id = 0;
    }
    std::string devpath = "/dev/phxfs_dev" + std::to_string(dev_id);
    printf("cuda gpu %d  ->  %s (dev_id=%d)\n", cuda_gpu_id, devpath.c_str(), dev_id);

    phase1_raw_mmap(devpath.c_str());

    if (do_phase2) {
        phase2_single_shot_regmem(devpath.c_str(), dev_id, cuda_gpu_id, allow_over_2g);
    } else {
        printf("\n[skip] Phase 2 not run. Add --phase2 to test the ioctl(MAP) insert path.\n");
        printf("       (Phase 2 may panic an UNPATCHED kernel module above 2GiB.)\n");
    }
    return 0;
}
