#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/time.h>
#include <linux/types.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>

#include "phoenix.h"

#define HUGE_PAGE_SIZE (64 * 1024)
#define SMALL_PAGE_SIZE (4 * 1024)
/*
 * Per-syscall I/O chunk size. The kernel module now supports a single
 * arbitrarily large mmap+ioctl registration (kvmalloc), so we no longer chunk
 * the *mapping*. We only chunk the *I/O* because a single read()/write()/
 * io_uring op transfers at most MAX_RW_COUNT (INT_MAX & PAGE_MASK = 0x7ffff000,
 * ~2GiB) per call. 1GiB is a clean, 64KiB-aligned value well under that cap.
 */
#define PHXFS_IO_CHUNK (1024ULL * 1024 * 1024)  /* 1 GiB */
#define PHXFS_MAX_DEVICES 8
#define QD 128

typedef struct phxfs_mmap_node_s {
    void *vaddr;        // single contiguous host-mapped P2P address
    uint64_t dev_addr;  // registered GPU/device address (lookup key)
    size_t length;      // registered length
    bool has_reg;
    struct phxfs_mmap_node_s *next; // 指向下一个节点的指针
} phxfs_p2p_map_t;

typedef struct phxfs_mmap_buffer_s {
    int device_id;
    int bdev_fd;
    bool init_stat;
    struct phxfs_mmap_node_s *head;
    pthread_mutex_t lock;
} phxfs_mmap_buffer_t;

static int g_device_count = PHXFS_MAX_DEVICES;
static phxfs_mmap_buffer_t mbuffer[PHXFS_MAX_DEVICES];
static std::vector<std::string> phxfs_dev_path = {
    "/dev/phxfs_dev0", "/dev/phxfs_dev1",
    "/dev/phxfs_dev2", "/dev/phxfs_dev3",
    "/dev/phxfs_dev4", "/dev/phxfs_dev5",
    "/dev/phxfs_dev6", "/dev/phxfs_dev7"
};

static std::vector<bool> phxfs_initialized(PHXFS_MAX_DEVICES, false);

void free_phxfs_p2p_map(phxfs_mmap_buffer_t *buffer) {
    struct phxfs_mmap_node_s *current = buffer->head;
    struct phxfs_mmap_node_s *next;
    int count = 0;
    pthread_mutex_lock(&buffer->lock);

    while (current) {
        next = current->next;
        // For nodes that are not pinned, manual release is required
        free(current);
        current = next;
        count++;
    }

    buffer->head = NULL;
    pthread_mutex_unlock(&buffer->lock);

}

static int __phxfs_close(phxfs_mmap_buffer_t *mbuffer) {
    if (!mbuffer->init_stat)
        return -1;
    if (mbuffer->bdev_fd > 0)
        close(mbuffer->bdev_fd);

    free_phxfs_p2p_map(mbuffer);
    pthread_mutex_destroy(&mbuffer->lock);
    return 0;
}

static int phxfs_close_all() {
    for (int i = 0; i < g_device_count; i++) {
        if (phxfs_initialized[i]) {
            __phxfs_close(&mbuffer[i]);
            phxfs_initialized[i] = false;
        }
    }
    return 0;
}

int phxfs_close(int device_id) {
    if (device_id >= 0 && device_id < g_device_count) {
        if (phxfs_initialized[device_id]) {
            __phxfs_close(&mbuffer[device_id]);
            phxfs_initialized[device_id] = false;
        }
        return 0;
    }
    return -1;
}

static int __phxfs_open(const char *dev_path, phxfs_mmap_buffer_t *mbuffer) {
    mbuffer->bdev_fd = open(dev_path, O_RDWR);
    
    if (mbuffer->bdev_fd == -1) {
        printf("failed to open file %s\n", dev_path);
        return -1;
    }
    mbuffer->head = NULL;
    pthread_mutex_init(&mbuffer->lock, NULL);
    mbuffer->init_stat = true;
    return 0;
}



bool is_phxfs_initialized() {
    bool initialized = false;
    for (int i = 0; i < g_device_count; i++) {
        initialized = initialized | phxfs_initialized[i];
    }
    return initialized;
}

int phxfs_open(int deviceID) {
    int ret;
    if (!is_phxfs_initialized()) {
        if (deviceID == -1) {
            for (int id = 0; id < g_device_count; ++id) {
                if (!phxfs_initialized[id]) {
                    ret = __phxfs_open(phxfs_dev_path[id].c_str(), &mbuffer[id]);
                    if (ret < 0) {
                        phxfs_close_all();
                        return ret;
                    }
                    phxfs_initialized[id] = true;
                }
            }
        } else if (deviceID >= 0 && deviceID < g_device_count) {
            if (!phxfs_initialized[deviceID]) {
                ret = __phxfs_open(phxfs_dev_path[deviceID].c_str(), &mbuffer[deviceID]);
                phxfs_initialized[deviceID] = true;
                return ret;
            }
        } else { // only start device id = 0
            if (!phxfs_initialized[0]) {
                ret = __phxfs_open(phxfs_dev_path[deviceID].c_str(), &mbuffer[0]);
                phxfs_initialized[0] = true;
                return ret;
            }
        }
    }
    return 0;
}


int phxfs_find_dev_for_cuda_gpu(int cuda_gpu_id) {
    // 1. Get PCI BDF of the CUDA GPU
    int cuda_domain, cuda_bus, cuda_device;
    cudaError_t err;

    err = cudaDeviceGetAttribute(&cuda_domain, cudaDevAttrPciDomainId, cuda_gpu_id);
    if (err != cudaSuccess) {
        fprintf(stderr, "%s: cudaDeviceGetAttribute(PciDomainId) failed for GPU %d\n",
                __func__, cuda_gpu_id);
        return -1;
    }
    err = cudaDeviceGetAttribute(&cuda_bus, cudaDevAttrPciBusId, cuda_gpu_id);
    if (err != cudaSuccess) {
        fprintf(stderr, "%s: cudaDeviceGetAttribute(PciBusId) failed for GPU %d\n",
                __func__, cuda_gpu_id);
        return -1;
    }
    err = cudaDeviceGetAttribute(&cuda_device, cudaDevAttrPciDeviceId, cuda_gpu_id);
    if (err != cudaSuccess) {
        fprintf(stderr, "%s: cudaDeviceGetAttribute(PciDeviceId) failed for GPU %d\n",
                __func__, cuda_gpu_id);
        return -1;
    }
    // GPUs are typically on PCI function 0
    int cuda_function = 0;

    // 2. Iterate over /sys/class/phxfs-generic/phxfs_dev*/pci_bdf to find a match
    for (int i = 0; i < PHXFS_MAX_DEVICES; i++) {
        std::string sysfs_path = "/sys/class/phxfs-generic/phxfs_dev"
                                 + std::to_string(i) + "/pci_bdf";
        std::ifstream ifs(sysfs_path);
        if (!ifs.is_open()) break;  // no more devices

        // Parse BDF string: "0000:a0:00.0\n"
        unsigned int domain, bus, device, function;
        char sep1, sep2, sep3;
        ifs >> std::hex >> domain >> sep1 >> bus >> sep2 >> device >> sep3 >> function;

        if (domain   == (unsigned int)cuda_domain &&
            bus      == (unsigned int)cuda_bus &&
            device   == (unsigned int)cuda_device &&
            function == (unsigned int)cuda_function) {
            return i;  // found matching phxfs_dev
        }
    }
    return -1;  // no matching phxfs device for this CUDA GPU
}


int insert_phxfs_mmap_node(phxfs_mmap_buffer_t *mbuffer, phxfs_p2p_map_t *new_node) {
    if (!new_node) {
        fprintf(stderr, "%s: new_node is NULL\n", __func__);
        return -1;
    }

    pthread_mutex_lock(&mbuffer->lock);
    new_node->next = mbuffer->head;
    mbuffer->head = new_node;
    pthread_mutex_unlock(&mbuffer->lock);
    return 0;
}

phxfs_p2p_map_t *find_phxfs_mmap_node(phxfs_mmap_buffer_t *mbuffer, u64 dev_addr, u64 len) {
    phxfs_p2p_map_t *current = mbuffer->head;
    while (current) {
        if (current->dev_addr <= dev_addr && ((current->dev_addr + current->length) >= (dev_addr + len))) {
            return current;
        }
        current = current->next;
    }
    return NULL; // 未找到节点
}

int delete_phxfs_mmap_node(phxfs_mmap_buffer_t *mbuffer, phxfs_p2p_map_t *p2p_map) {
    phxfs_p2p_map_t *current = mbuffer->head;
    phxfs_p2p_map_t *previous = NULL;
    pthread_mutex_lock(&mbuffer->lock);
    while (current) {
        if (current->dev_addr == p2p_map->dev_addr) {
            if (previous) {
                previous->next = current->next;
            } else {
                mbuffer->head = current->next;
            }
            pthread_mutex_unlock(&mbuffer->lock);
            return 0;
        }
        previous = current;
        current = current->next;
    }
    pthread_mutex_unlock(&mbuffer->lock);
    return -1;
}

static inline int __phxfs_regmem(phxfs_mmap_buffer_t *mbuffer, u64 dev_addr, u64 c_addr, size_t len) {
    phxfs_ioctl_para_t para;
    int ret;

    if (!mbuffer->init_stat) {
        return -1;
    }

    para.map_param.n_vaddr = (u64)dev_addr;
    para.map_param.c_vaddr = (u64)c_addr;
    para.map_param.c_size = len;
    para.map_param.n_size = len;
    para.map_param.dev.dev_id = mbuffer->device_id;
    ret = ioctl(mbuffer->bdev_fd, PHXFS_IOCTL_MAP, &para);

    return ret;
}

int phxfs_regmem(int device_id, const void *addr, size_t len, void **target_addr) {
    int ret = 0;

    phxfs_mmap_buffer_t *pb = &mbuffer[device_id];
    phxfs_p2p_map_t *p2p_map = (phxfs_p2p_map_t *)malloc(sizeof(phxfs_p2p_map_t));
    if (!p2p_map) {
        fprintf(stderr, "%s: new_node is NULL\n", __func__);
        return -1;
    }

    if (len % HUGE_PAGE_SIZE != 0) {
        fprintf(stderr, "%s: len is not 64KiB aligned\n", __func__);
        free(p2p_map);
        return -EFAULT;
    }

    p2p_map->vaddr = NULL;
    p2p_map->length = len;
    p2p_map->dev_addr = (uint64_t)addr;
    p2p_map->has_reg = 0;
    p2p_map->next = NULL;
    pb->device_id = device_id;

    /*
     * Single contiguous mmap of the whole region. The phxfs mmap backend is
     * lazy (no size-proportional allocation), and the ioctl insert path now
     * uses kvmalloc, so there is no longer a 2GiB-per-mmap limit and no need
     * to split into chunks.
     */
    p2p_map->vaddr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, pb->bdev_fd, 0);
    if (p2p_map->vaddr == MAP_FAILED) {
        fprintf(stderr, "%s: mmap fail (%s)\n", __func__, strerror(errno));
        free(p2p_map);
        return -EFAULT;
    }

    ret = __phxfs_regmem(pb, p2p_map->dev_addr, (uint64_t)p2p_map->vaddr, len);
    if (ret) {
        fprintf(stderr, "%s: __phxfs_regmem fail ret=%d (%s)\n", __func__, ret, strerror(errno));
        munmap(p2p_map->vaddr, len);
        free(p2p_map);
        return -EFAULT;
    }

    p2p_map->has_reg = 1;
    insert_phxfs_mmap_node(pb, p2p_map);
    *target_addr = p2p_map->vaddr;

    return 0;
}


int __phxfs_deregmem(phxfs_mmap_buffer_t *pb, u64 dev_addr, u64 c_addr, size_t len) {
    phxfs_ioctl_para_t para;
    int ret;

    if(!pb->init_stat) {
        return -1;
    }

    para.map_param.n_vaddr = (u64)dev_addr;
    para.map_param.c_vaddr = (u64)c_addr;
    para.map_param.c_size = len;
    para.map_param.n_size = len;
    para.map_param.dev.dev_id = pb->device_id;

    // print_mem(&para, sizeof(phxfs_ioctl_para_t));
    ret = ioctl(pb->bdev_fd, PHXFS_IOCTL_UNMAP, &para);

    return ret;
}

int phxfs_deregmem(int device_id, const void *addr, size_t len) {
    int ret = 0;
    phxfs_p2p_map_t *p2p_map;
    phxfs_mmap_buffer_t *pb = &mbuffer[device_id];

    p2p_map = find_phxfs_mmap_node(pb, (u64)addr, len);
    if (p2p_map == NULL) {
        fprintf(stderr, "%s: p2p_map is not found fail\n", __func__);
        return -1;
    }

    if (p2p_map->length != len || !p2p_map->has_reg) {
        fprintf(stderr, "%s: p2p_map is not match or has not reg\n", __func__);
        return -1;
    }

    ret = delete_phxfs_mmap_node(pb, p2p_map);
    if (ret) {
        fprintf(stderr, "%s: delete_phxfs_mmap_node fail! not found the map\n", __func__);
        return -1;
    }

    ret = __phxfs_deregmem(pb, p2p_map->dev_addr, (uint64_t)p2p_map->vaddr, p2p_map->length);
    if (ret) {
        fprintf(stderr, "%s: __phxfs_deregmem fail\n", __func__);
        free(p2p_map);
        return -1;
    }

    ret = munmap(p2p_map->vaddr, p2p_map->length);
    if (ret) {
        fprintf(stderr, "%s: munmap fail (%s)\n", __func__, strerror(errno));
        free(p2p_map);
        return -1;
    }

    free(p2p_map);
    return ret;
}

int phxfs_close(phxfs_fileid_t fid) {
    return close(fid.fd);
}

// Resolve a registered GPU buffer to its contiguous host-mapped P2P address.
// Returns vaddr + buf_offset, or NULL if not registered / out of range.
static void *phxfs_resolve_target(int device_id, const void *buf, off_t buf_offset, size_t nbyte) {
    phxfs_mmap_buffer_t *pb = &mbuffer[device_id];
    phxfs_p2p_map_t *p2p_map = find_phxfs_mmap_node(pb, (u64)buf, nbyte);

    if (!p2p_map) {
        fprintf(stderr, "%s: p2p_map not found\n", __func__);
        return NULL;
    }
    if (!p2p_map->has_reg) {
        fprintf(stderr, "%s: p2p_map not registered\n", __func__);
        return NULL;
    }
    if ((size_t)(nbyte + buf_offset) > p2p_map->length) {
        fprintf(stderr, "%s: out of range: nbyte=%zu buf_offset=%ld length=%zu\n",
                __func__, nbyte, (long)buf_offset, p2p_map->length);
        return NULL;
    }
    return (void *)((char *)p2p_map->vaddr + buf_offset);
}

ssize_t phxfs_read(phxfs_fileid_t fid, void *buf, off_t buf_offset, ssize_t nbyte, off_t f_offset){
    char *base = (char *)phxfs_resolve_target(fid.deviceID, buf, buf_offset, nbyte);
    ssize_t nbyte_total = 0;

    if (base == NULL)
        return -1;

    // Single contiguous mapping; chunk only to stay under MAX_RW_COUNT (~2GiB).
    while (nbyte_total < nbyte) {
        size_t this_nbyte = (size_t)(nbyte - nbyte_total);
        if (this_nbyte > PHXFS_IO_CHUNK)
            this_nbyte = PHXFS_IO_CHUNK;
        ssize_t ret = pread(fid.fd, base + nbyte_total, this_nbyte, f_offset + nbyte_total);
        if (ret < 0) {
            fprintf(stderr, "%s: pread error: %s\n", __func__, strerror(errno));
            return -1;
        }
        if (ret == 0)
            break; // EOF
        nbyte_total += ret;
    }
    return nbyte_total;
}

ssize_t phxfs_write(phxfs_fileid_t fid, void *buf, off_t buf_offset, ssize_t nbyte, off_t f_offset){
    char *base = (char *)phxfs_resolve_target(fid.deviceID, buf, buf_offset, nbyte);
    ssize_t nbyte_total = 0;

    if (base == NULL)
        return -1;

    while (nbyte_total < nbyte) {
        size_t this_nbyte = (size_t)(nbyte - nbyte_total);
        if (this_nbyte > PHXFS_IO_CHUNK)
            this_nbyte = PHXFS_IO_CHUNK;
        ssize_t ret = pwrite(fid.fd, base + nbyte_total, this_nbyte, f_offset + nbyte_total);
        if (ret < 0) {
            fprintf(stderr, "%s: pwrite error: %s\n", __func__, strerror(errno));
            return -1;
        }
        if (ret == 0)
            break;
        nbyte_total += ret;
    }
    return nbyte_total;
}

