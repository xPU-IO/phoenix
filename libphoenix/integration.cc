#include <cstddef>
#include <cstdint>
#include <cuda_runtime.h>
#include <liburing.h>
#include <sys/types.h>
#include <unistd.h>

#include "phoenix.h"


enum phxfs_op {
    PHXFS_OP_READ = 0,
    PHXFS_OP_WRITE = 1,
};

struct phxfs_data {
    phxfs_fileid_t fid;
    int op;
    void *buf;
    size_t nbytes;
    off_t file_offset;
    ssize_t *bytes_done;
};

// Runs on the CUDA stream's host-callback thread. Reuses the synchronous
// phxfs_read/write (single contiguous mapping + per-syscall chunking).
void CUDART_CB phxfs_callback(void *user_data){
    auto* data = static_cast<phxfs_data*>(user_data);
    if (data->op == PHXFS_OP_READ)
        *data->bytes_done = phxfs_read(data->fid, data->buf, 0, (ssize_t)data->nbytes, data->file_offset);
    else
        *data->bytes_done = phxfs_write(data->fid, data->buf, 0, (ssize_t)data->nbytes, data->file_offset);
    delete data;
}


cudaError_t phxfs_async(phxfs_fileid_t fid, enum phxfs_op op,
                            void* buf,
                            size_t nbytes, off_t offset,
                            ssize_t *bytes_done,
                            CUstream stream){
    auto* data = new phxfs_data{
        .fid = fid, .op = op, .buf = buf,
        .nbytes = nbytes, .file_offset = offset,
        .bytes_done = bytes_done
    };
    return cudaLaunchHostFunc(stream, phxfs_callback, data);
}

cudaError_t phxfs_read_async(phxfs_fileid_t fid,
                            void* buf,
                            size_t nbytes, off_t offset,
                            ssize_t *bytes_done,
                            CUstream stream) {
    return phxfs_async(fid, PHXFS_OP_READ, buf, nbytes, offset, bytes_done, stream);
}

cudaError_t phxfs_write_async(phxfs_fileid_t fid,
                             void* buf, 
                             size_t nbytes, off_t offset,
                             ssize_t* bytes_done,
                             CUstream stream) {
    return phxfs_async(fid, PHXFS_OP_WRITE, buf, nbytes, offset, bytes_done, stream);
}
