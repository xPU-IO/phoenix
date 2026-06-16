// test_regmem_io.cu
//
// 验证简化后(单次 mmap) 的 libphoenix 在 >2GB 单次注册下的【数据正确性】+【读吞吐】。
//   - 数据文件采用确定性模式：每 8 字节存其自身文件偏移量(uint64, LE)。
//   - 文件按需准备：存在且够大则原样复用；不存在则创建；偏小则只补写缺失尾部。
//   - phxfs_read(file O_DIRECT -> GPU P2P) 计时统计吞吐；再 cudaMemcpy 回 host 逐 8 字节校验。
//
// 编译：
//   nvcc -O2 -std=c++17 /data/home/ryeqiu/phoenix/tests/test_regmem_io.cu \
//        -I /data/home/ryeqiu/phoenix/libphoenix/include \
//        -L /data/home/ryeqiu/phoenix/build -lphoenix -lcudart \
//        -Xlinker -rpath=/data/home/ryeqiu/phoenix/build -o /tmp/test_regmem_io
//
// 运行：
//   /tmp/test_regmem_io 0          # GPU0，默认 3GiB
//   /tmp/test_regmem_io 0 16       # 16GiB
//   文件落在 /mnt/raid1/phxfs_io_test.bin

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>

#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include <cuda_runtime.h>
#include "phoenix.h"

#define GIB (1024ULL * 1024 * 1024)

static const char *FILE_PATH = "/mnt/raid1/phxfs_io_test.bin";

static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// 确保 FILE_PATH 至少有 size 字节、且 [0,size) 是 8 字节偏移量模式。
// 存在且够大 -> 不动；不存在 -> 创建; 偏小 -> 只补写 [cur, size) 尾部。
static int ensure_pattern_file(size_t size, uint64_t *hbuf) {
    struct stat st;
    off_t cur = (stat(FILE_PATH, &st) == 0) ? st.st_size : -1;

    if (cur >= (off_t)size) {
        printf("[ok] file exists, size %lld B (%.2f GiB) >= need %.2f GiB, reuse as-is\n",
               (long long)cur, (double)cur / GIB, (double)size / GIB);
        return 0;
    }

    off_t start = (cur > 0) ? (cur / 8 * 8) : 0;  // 8 字节对齐起点
    // 填充绝对偏移量模式（仅尾部要写，但整段填好便于按绝对偏移取值）
    for (size_t i = start / 8; i < size / 8; i++)
        hbuf[i] = (uint64_t)(i * 8);

    int wfd = open(FILE_PATH, O_WRONLY | O_CREAT, 0644);
    if (wfd < 0) { printf("open(write) failed: %s\n", strerror(errno)); return -1; }

    size_t done = (size_t)start;
    const char *p = (const char *)hbuf;
    while (done < size) {
        ssize_t w = pwrite(wfd, p + done, size - done, done);
        if (w <= 0) { printf("pwrite failed: %s\n", strerror(errno)); close(wfd); return -1; }
        done += (size_t)w;
    }
    fsync(wfd);
    posix_fadvise(wfd, 0, size, POSIX_FADV_DONTNEED);
    close(wfd);

    printf("[ok] %s pattern file [%lld, %zu) (had %lld B)\n",
           (cur < 0) ? "created" : "extended", (long long)start, size, (long long)cur);
    return 0;
}

int main(int argc, char **argv) {
    int cuda_gpu_id = (argc > 1) ? atoi(argv[1]) : 0;
    double gib = (argc > 2) ? atof(argv[2]) : 3.0;
    size_t size = (size_t)(gib * GIB);
    size &= ~(64ULL * 1024 - 1);  // 64KiB 对齐

    printf("GPU %d, size = %.2f GiB (%zu bytes), file = %s\n", cuda_gpu_id, gib, size, FILE_PATH);

    if (cudaSetDevice(cuda_gpu_id) != cudaSuccess) { printf("cudaSetDevice failed\n"); return 1; }
    int dev_id = phxfs_find_dev_for_cuda_gpu(cuda_gpu_id);
    if (dev_id < 0) { printf("no phxfs dev for gpu %d\n", cuda_gpu_id); return 1; }
    printf("phxfs dev_id = %d\n", dev_id);

    // ---- 1) host 缓冲（兼作模式生成源 + 校验目的地） + 准备文件 ----
    uint64_t *hbuf = (uint64_t *)malloc(size);
    if (!hbuf) { printf("host malloc %.2fGiB failed\n", gib); return 1; }
    if (ensure_pattern_file(size, hbuf) != 0) return 1;

    // ---- 2) cudaMalloc + phxfs_regmem（单次 mmap，>2GB） ----
    void *gbuf = nullptr, *target = nullptr;
    if (cudaMalloc(&gbuf, size) != cudaSuccess) { printf("cudaMalloc failed\n"); return 1; }
    cudaMemset(gbuf, 0, size);
    cudaStreamSynchronize(0);

    if (phxfs_open(dev_id) != 0) { printf("phxfs_open failed\n"); return 1; }
    int ret = phxfs_regmem(dev_id, gbuf, size, &target);
    if (ret) { printf("phxfs_regmem failed: %d\n", ret); return 1; }
    printf("[ok] phxfs_regmem single-shot %.2fGiB, target=%p\n", gib, target);

    // ---- 3) phxfs_read: NVMe(O_DIRECT) -> GPU P2P，计时统计吞吐 ----
    int dfd = open(FILE_PATH, O_RDONLY | O_DIRECT);
    if (dfd < 0) { printf("open(O_DIRECT) failed: %s\n", strerror(errno)); return 1; }

    double t0 = now_sec();
    ssize_t rd = phxfs_read({.fd = dfd, .deviceID = dev_id}, gbuf, 0, size, 0);
    double t1 = now_sec();
    close(dfd);

    if (rd != (ssize_t)size) { printf("phxfs_read returned %zd, expected %zu\n", rd, size); return 1; }

    double secs = t1 - t0;
    double gbps  = (double)size / secs / 1e9;          // GB/s (10^9)
    double gibps = (double)size / secs / (double)GIB;  // GiB/s (2^30)
    printf("[ok] phxfs_read %zd bytes in %.3f ms  ->  %.2f GB/s (%.2f GiB/s)\n",
           rd, secs * 1e3, gbps, gibps);

    // ---- 4) GPU -> host，逐 8 字节校验 ----
    memset(hbuf, 0xFF, size);
    if (cudaMemcpy(hbuf, gbuf, size, cudaMemcpyDeviceToHost) != cudaSuccess) { printf("cudaMemcpy D2H failed\n"); return 1; }
    cudaStreamSynchronize(0);

    // ---- 4a) 字校验：每 8 字节(uint64) == 其文件偏移量 ----
    size_t word_errors = 0; long long first_word_err = -1;
    for (size_t i = 0; i < size / 8; i++) {
        if (hbuf[i] != (uint64_t)(i * 8)) {
            if (first_word_err < 0) first_word_err = (long long)(i * 8);
            if (word_errors < 5)
                printf("  WORD MISMATCH off=0x%zx expected=0x%zx got=0x%lx\n", i * 8, i * 8, hbuf[i]);
            word_errors++;
        }
    }
    printf("[%s] word-level check: %zu mismatch(es)\n", word_errors ? "FAIL" : "ok", word_errors);

    // ---- 4b) 逐字节校验：窗口化 memcmp，保证每一个字节都正确 ----
    const size_t WIN = 64ULL * 1024 * 1024;  // 64MiB 暂存窗口（8 对齐）
    uint8_t *expect = (uint8_t *)malloc(WIN);
    if (!expect) { printf("expect malloc failed\n"); return 1; }
    size_t byte_errors = 0; long long first_byte_err = -1;
    for (size_t off = 0; off < size; off += WIN) {
        size_t win = (size - off < WIN) ? (size - off) : WIN;
        uint64_t *ew = (uint64_t *)expect;
        for (size_t j = 0; j < win / 8; j++) ew[j] = (uint64_t)(off + j * 8);  // 期望窗口(按字快速生成)
        const uint8_t *got = (const uint8_t *)hbuf + off;
        if (memcmp(got, expect, win) != 0) {                                   // 整窗逐字节比对
            for (size_t k = 0; k < win; k++) {
                if (got[k] != expect[k]) {
                    if (first_byte_err < 0) first_byte_err = (long long)(off + k);
                    if (byte_errors < 5)
                        printf("  BYTE MISMATCH off=0x%zx expected=0x%02x got=0x%02x\n",
                               off + k, expect[k], got[k]);
                    byte_errors++;
                }
            }
        }
    }
    free(expect);
    printf("[%s] per-byte check: %zu byte mismatch(es)\n", byte_errors ? "FAIL" : "ok", byte_errors);

    size_t errors = word_errors + byte_errors;
    if (errors == 0)
        printf("PASSED: all %zu bytes verified (word + per-byte) across single-shot %.2fGiB registration\n", size, gib);
    else
        printf("FAILED: word_err=%zu (first 0x%llx), byte_err=%zu (first 0x%llx)\n",
               word_errors, (unsigned long long)(first_word_err < 0 ? 0 : first_word_err),
               byte_errors, (unsigned long long)(first_byte_err < 0 ? 0 : first_byte_err));

    phxfs_deregmem(dev_id, gbuf, size);
    phxfs_close(dev_id);
    cudaFree(gbuf);
    free(hbuf);
    return errors ? 1 : 0;
}
