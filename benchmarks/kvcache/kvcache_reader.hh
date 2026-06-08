// batch_reader.hh
#ifndef __KVCACHE_READER_HH__
#define __KVCACHE_READER_HH__

#include <vector>
#include <string>
#include <liburing.h>
#include <cuda.h>
#include <cufile.h>
#include <fcntl.h>

struct Sequence {
    std::string id;
    std::vector<size_t> block_ids;
};

class BaseKVCacheReader {
public:
    virtual void load_sequences(const std::string& trace_file) = 0;
    virtual void process_all_sequences() = 0;
    virtual ~BaseKVCacheReader() {}
};

class PhxfsKVCacheReader : public BaseKVCacheReader {
public:
    PhxfsKVCacheReader(size_t max_batch_size = 2048, int gpu_id = -1, int phxfs_dev_id = -1);
    ~PhxfsKVCacheReader();
    void load_sequences(const std::string& trace_file) override;
    void process_all_sequences() override;

private:
    int fd, gpu_id, phxfs_dev_id;
    void **devPtrs;
    void **host_ptrs;
    std::vector<Sequence> sequences;
    size_t max_batch_size;
};

class KVCacheNativeReader : public BaseKVCacheReader {
public:
    KVCacheNativeReader(size_t max_batch_size = 2048, int device_id = -1);
    ~KVCacheNativeReader();
    void load_sequences(const std::string& trace_file) override;
    void process_all_sequences() override;

private:
    int fd, device_id;
    void **devPtrs;
    void **cpuPtrs;
    std::vector<Sequence> sequences;
    size_t max_batch_size;
};

class CuFileKVCacheReader : public BaseKVCacheReader {
public:
    CuFileKVCacheReader(size_t max_batch_size = 256, int device_id = -1);
    ~CuFileKVCacheReader();
    void load_sequences(const std::string& trace_file) override;
    void process_all_sequences() override;

private:
    CUfileHandle_t cf_handle;
    CUfileBatchHandle_t batch_id;
    int fd;
    int device_id;
    void **devPtrs;
    std::vector<Sequence> sequences;
    size_t max_batch_size;
};

#endif // __KVCACHE_READER_HH__