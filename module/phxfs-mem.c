#include <asm/page_types.h>
#include <linux/kernel.h> 
#include <linux/blkdev.h> 
#include <linux/blk_types.h> 
#include <linux/random.h> 
#include <linux/file.h> 
#include <linux/hash.h> 

#include <linux/memory.h> 
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

 
#include "phxfs-mem.h"  
#include "nvfs-core.h"
#include "nvfs-p2p.h"
#include "config-host.h"

static DEFINE_HASHTABLE(phxfs_io_mbuffer_hash, PHXFS_MAX_SHADOW_ALLOCS_ORDER); 
static spinlock_t lock ____cacheline_aligned; 
atomic_t base_index_cnt = ATOMIC_INIT(0);

/*
 * Look up the virtual address for a BAR offset using multi-segment mapping.
 * Uses binary search on segments array for efficiency.
 * For legacy single-segment devices (segments == NULL), falls back to
 * pci_mem_va + bar_offset.
 */
void *phxfs_bar_offset_to_va(struct phxfs_dev *dev, u64 bar_offset)
{
	int lo, hi, mid;
	u64 phys_addr;

	if (!dev)
		return NULL;

	/* Legacy single-segment path */
	if (!dev->segments || dev->num_segments <= 0) {
		if (dev->pci_mem_va && bar_offset < dev->size)
			return dev->pci_mem_va + bar_offset;
		return NULL;
	}

	phys_addr = dev->paddr + bar_offset;

	/* Binary search: find the segment containing phys_addr */
	lo = 0;
	hi = dev->num_segments - 1;
	while (lo <= hi) {
		u64 seg_start, seg_end;
		mid = lo + (hi - lo) / 2;
		seg_start = dev->segments[mid].phys_start;
		seg_end = seg_start + dev->segments[mid].size;

		if (phys_addr < seg_start) {
			hi = mid - 1;
		} else if (phys_addr >= seg_end) {
			lo = mid + 1;
		} else {
			/* Found: phys_addr is within [seg_start, seg_end) */
			return dev->segments[mid].va + (phys_addr - seg_start);
		}
	}

	return NULL;
}

void unmap_and_release(struct p2p_vmap* map)
{
    if (!map)
        return;

    if (map->release != NULL && map->data != NULL)
    {
        map->release(map);
    }

    kfree(map);
	phxfs_info("unmap_and_release\n");
}


void release_gpu_memory(struct p2p_vmap* map)
{
    struct gpu_region* gd;

    if (!map)
        return;

    gd = (struct gpu_region*) map->data;
    if (gd != NULL)
    {
        if (gd->pages != NULL)
        {
            nvfs_nvidia_p2p_put_pages(0, 0, map->gpuvaddr, gd->pages);
        }
        kfree(gd);
        map->data = NULL;
    }
	if(map->pages!=NULL)
	{
		kfree(map->pages);
	}
	if(map!=NULL)
		kfree(map);
}

static void force_release_gpu_memory(struct p2p_vmap* map)
{
    struct gpu_region* gd;

    if (WARN_ON(!map))
        return;

    gd = (struct gpu_region*) map->data;


    if (gd != NULL)
    {

        if (gd->pages != NULL)
        {
            nvfs_nvidia_p2p_put_pages(0, 0, map->gpuvaddr, gd->pages);
        }

        kfree(gd);
        map->data = NULL;

        phxfs_warn("Nvidia driver forcefully reclaimed %lu GPU pages\n", map->n_addrs);
    	
	}
	unmap_and_release(map);
}

phxfs_mmap_buffer_t phxfs_check_and_bind_phony_buffer(u64 cpuvaddr, u64 length) { 
    phxfs_mmap_buffer_t mbuffer = NULL; 
    struct mm_struct *mm = current->mm; 
    struct vm_area_struct *vma; 

    if (!cpuvaddr) { 
        phxfs_err("phxfs_check_and_bind_phony_buffer get cpuvaddr error\n");
        goto out; 
    } 

    if (cpuvaddr % PAGE_SIZE) { 
        phxfs_err("phxfs_check_and_bind_phony_buffer cpuvaddr not aligned\n");
        goto out; 
    } 

    vma = find_vma(mm, cpuvaddr); 
    if (vma == NULL)
        goto out; 
        

    mbuffer = (phxfs_mmap_buffer_t)vma->vm_private_data; 
    if (mbuffer!= NULL) { 
        if (mbuffer->c_vaddr!= cpuvaddr || mbuffer->map_len!= length) { 
            phxfs_warn("reg region is not same as mmap region\n");
            goto out; 
        } else { 
            return mbuffer; 
        } 
    } else { 
        phxfs_warn("vma found, but mbuffer is none!\n");
        goto out; 
    } 

out: 
    return NULL; 
} 

int phxfs_map_dev_addr_inner(phxfs_mmap_buffer_t mbuffer, u64 devaddr, u64 dev_len) {
    struct phxfs_dev *dev = NULL;
    struct gpu_region* gd = NULL;
    struct vm_area_struct *vma;
    u64 *dev_page_addrs = NULL;
    u64 page_size;
    u64 nr_dev_pages;
    u64 pci_bar_off;
    u64 cpu_vaddr;
    unsigned long total_pages;
    unsigned long k;
    bool pages_pinned = false;
    int ret = 0, i, j;

    if (!mbuffer)
        return -EINVAL;

    vma = mbuffer->vma;
    page_size = GPU_PAGE_SIZE;
    mbuffer->subpage_num = page_size / PAGE_SIZE;
    dev = mbuffer->dev;
    
    if (dev == NULL || dev->pci_mem_va == NULL) {
        phxfs_err("phxfs_map_dev_addr_inner get npu info error\n");
        ret = -ENOMEM;
        goto out;
    }
    
    nr_dev_pages = DIV_ROUND_UP(dev_len, page_size);

    mbuffer->dev_page_num = nr_dev_pages;
    if (dev_len < GPU_PAGE_SIZE) {
        if (dev_len % PAGE_SIZE != 0){
            ret = -EINVAL;
            goto out;
        }
        mbuffer->host_page_num = DIV_ROUND_UP(dev_len, PAGE_SIZE);
    }else{
        mbuffer->host_page_num = nr_dev_pages * (mbuffer->subpage_num);
    }
    
    /*
     * The arrays below scale linearly with the registered region size.
     * A single kmalloc() is capped at KMALLOC_MAX_SIZE (~4MiB of physically
     * contiguous memory). ppages needs region/512 bytes, hitting 4MiB exactly
     * at a 2GiB registration -- this is the real root cause of the historical
     * "2GB per mmap" limit. Use kvmalloc() so large arrays transparently fall
     * back to vmalloc() and the limit becomes the BAR/HBM size instead.
     */
    dev_page_addrs = kvcalloc(nr_dev_pages, sizeof(u64), GFP_KERNEL);
    if (dev_page_addrs == NULL) {
        ret = -ENOMEM;
        goto out;
    }

    mbuffer->ppages = (struct page **) kvmalloc_array(mbuffer->host_page_num,
                                                      sizeof(struct page *), GFP_KERNEL);
    if (mbuffer->ppages == NULL) {
        phxfs_err("Failed to allocate ppages array (%lu entries)\n", mbuffer->host_page_num);
        ret = -ENOMEM;
        goto out;
    }

    /*
     * map descriptor scales as region/8192, only exceeds kmalloc above ~32GiB;
     * kept on kmalloc so release_gpu_memory()'s kfree() path stays valid.
     */
    mbuffer->map = kmalloc(sizeof(struct p2p_vmap) + (nr_dev_pages - 1) * sizeof(uint64_t), GFP_KERNEL);
    if (mbuffer->map == NULL)
    {
        phxfs_err("Failed to allocate mapping descriptor\n");
        ret = -ENOMEM;
        goto out;
    }

    mbuffer->map->page_size = GPU_PAGE_SIZE;
    mbuffer->map->release = release_gpu_memory;
    mbuffer->map->size = dev_len;
    mbuffer->map->gpuvaddr = devaddr;
    mbuffer->map->n_addrs = mbuffer->dev_page_num;
    mbuffer->map->pages = NULL;
    for (i = 0; i < mbuffer->map->n_addrs; ++i)
    {
        mbuffer->map->addrs[i] = 0;
    } 
    gd = kmalloc(sizeof(struct gpu_region), GFP_KERNEL);
    if (gd == NULL)
    {
        phxfs_err("Failed to allocate gpu_region\n");
        ret = -ENOMEM;
        goto out;
    }
    gd->pages = NULL;
    mbuffer->map->data = (struct gpu_region*)gd;
    ret = nvfs_nvidia_p2p_get_pages(0, 0, mbuffer->map->gpuvaddr, GPU_PAGE_SIZE * mbuffer->map->n_addrs, &gd->pages, 
        (void (*)(void*)) force_release_gpu_memory, mbuffer->map);
    if (ret != 0 || gd->pages == NULL) {
        phxfs_err("nvfs_nvidia_p2p_get_pages failed, ret=%d\n", ret);
        if (ret == 0)
            ret = -ENOMEM;
        goto out;
    }
    pages_pinned = true;

    for(i = 0; i < mbuffer->map->n_addrs; i++)
    {
        if(gd->pages->pages[i]==NULL)
        {
            phxfs_err("mem allocation not success, i is %d!\n",i);
            ret = -ENOMEM;
            goto out;
        }
        dev_page_addrs[i] = gd->pages->pages[i]->physical_address;
    }

    mbuffer->dev_page_addrs = dev_page_addrs;
    total_pages = mbuffer->host_page_num;
    if (IS_ERR_OR_NULL(mbuffer->ppages)) {
        ret = -ENOMEM;
        goto out;
    }
    
    for (i = 0; i < nr_dev_pages; i++) {
        pci_bar_off = dev_page_addrs[i] - mbuffer->dev->paddr;
        cpu_vaddr = (uint64_t)phxfs_bar_offset_to_va(mbuffer->dev, pci_bar_off);
        if (cpu_vaddr == 0) {
            phxfs_err("phxfs_map_dev_addr_inner: bar_offset 0x%llx not in any segment\n",
                   pci_bar_off);
            ret = -EFAULT;
            goto out;
        }
        for (j = 0; j < mbuffer->subpage_num; j++) {
            mbuffer->ppages[i * mbuffer->subpage_num + j] = virt_to_page(cpu_vaddr + j * PAGE_SIZE);
        }
    }


    for (k = 0; k < total_pages; k++) {
        ret = vm_insert_page(vma,
                             mbuffer->c_vaddr + k * PAGE_SIZE,
                             mbuffer->ppages[k]);
        if (ret) {
            phxfs_err("vm_insert_page failed, k=%lu, ret=%d\n", k, ret);
            goto out;
        }
    }

    /**

    ret = vm_insert_pages(vma, mbuffer->c_vaddr, mbuffer->ppages, &total_pages);
    
    if (ret && total_pages) {
        printk("vm_insert_pages failed\n");
        goto out;
    }
    **/
    mbuffer->remap = 1;
    return 0;

out:
    /*
     * Error teardown. If GPU pages were pinned, release them ourselves
     * (nvfs_nvidia_p2p_put_pages() does not invoke the registered
     * force-release callback), then free the descriptors. This fixes the
     * previous NULL-deref / use-after-free that panicked the kernel when a
     * registration above ~2GiB failed (ppages kmalloc returned NULL and the
     * unchecked get_pages / out: path freed the still-referenced map & gd).
     */
    if (pages_pinned && gd != NULL && gd->pages != NULL) {
        nvfs_nvidia_p2p_put_pages(0, 0, devaddr, gd->pages);
        gd->pages = NULL;
    }
    if (gd != NULL) {
        kfree(gd);
        if (mbuffer->map != NULL)
            mbuffer->map->data = NULL;
    }
    if (mbuffer->map != NULL) {
        kfree(mbuffer->map);
        mbuffer->map = NULL;
    }
    if (mbuffer->ppages != NULL) {
        kvfree(mbuffer->ppages);
        mbuffer->ppages = NULL;
    }
    if (dev_page_addrs != NULL) {
        kvfree(dev_page_addrs);
        mbuffer->dev_page_addrs = NULL;
    }

    return ret;
}

int phxfs_map_dev_addr(phxfs_ioctl_map_t *map_param, u64 devaddr, u64 dev_len, u64 cpuvaddr, u64 length) {
    int ret = -EINVAL;
    phxfs_mmap_buffer_t mbuffer;
    
    mbuffer = phxfs_check_and_bind_phony_buffer(cpuvaddr, length);
    if (mbuffer == NULL || mbuffer->vma == NULL || devaddr <= length) {
        return ret;
    } else {
        ret = 0;
        mbuffer->n_vaddr = devaddr;
        mbuffer->dev_len = dev_len;
        ret = phxfs_map_dev_addr_inner(mbuffer, devaddr, dev_len);
        return ret;
    }
}

void phxfs_mbuffer_put(phxfs_mmap_buffer_t mbuffer);
void phxfs_map_dev_release(phxfs_ioctl_map_t *map_param, u64 devaddr, u64 dev_len, u64 cpuvaddr, u64 length) {
    phxfs_mmap_buffer_t mbuffer;
    mbuffer = phxfs_check_and_bind_phony_buffer(cpuvaddr, length);
    phxfs_mbuffer_put(mbuffer);
}

static void phxfs_mbuffer_free(phxfs_mmap_buffer_t mbuffer) {
    if (WARN_ON(!mbuffer))
        return;

    spin_lock(&lock);
    hash_del_rcu(&mbuffer->hash_link);
    spin_unlock(&lock);
    
    if (mbuffer->remap) {
        release_gpu_memory(mbuffer->map);
    }

    if (mbuffer->dev_page_addrs != NULL) {
        kvfree(mbuffer->dev_page_addrs);
    }
    if (mbuffer->ppages != NULL) {
        kvfree(mbuffer->ppages);
    }
    
    mbuffer->dev = NULL;
    mbuffer->vma = NULL;
    mbuffer->base_index = 0;
}

void phxfs_mbuffer_init(void) {
    spin_lock_init(&lock);
    hash_init(phxfs_io_mbuffer_hash);
}


void phxfs_mbuffer_get_ref(phxfs_mmap_buffer_t mbuffer) {
    if (WARN_ON(!mbuffer))
        return;
    atomic_inc(&mbuffer->ref);
}

bool phxfs_mbuffer_put_ref(phxfs_mmap_buffer_t mbuffer) {
    if (WARN_ON(!mbuffer))
        return true;
    return atomic_dec_and_test(&mbuffer->ref);
}

static void phxfs_mbuffer_put_internal(phxfs_mmap_buffer_t mbuffer) {
    if (mbuffer == NULL) return;
    
    if (phxfs_mbuffer_put_ref(mbuffer)) {
        phxfs_mbuffer_free(mbuffer);
        kfree(mbuffer);
    }
}

void phxfs_mbuffer_put(phxfs_mmap_buffer_t mbuffer) {
    return phxfs_mbuffer_put_internal(mbuffer);
}

void phxfs_mbuffer_put_dma(phxfs_mmap_buffer_t mbuffer) {
    return phxfs_mbuffer_put_internal(mbuffer);
}

// 代码段开始

// 获取未加锁的phony缓冲区
static inline phxfs_mmap_buffer_t phxfs_mbuffer_get_unlocked(unsigned long base_index) {
    phxfs_mmap_buffer_t phxfs_mbuffer;
    hash_for_each_possible_rcu(phxfs_io_mbuffer_hash, phxfs_mbuffer, hash_link, base_index) {
        if (phxfs_mbuffer->base_index == base_index) {
            phxfs_mbuffer_get_ref(phxfs_mbuffer);
            return phxfs_mbuffer;
        }
    }
    // printk("base_index %lx not found \n", base_index);
    return NULL;
}

// 获取phony缓冲区
phxfs_mmap_buffer_t phxfs_mbuffer_get(unsigned long base_index) {
    phxfs_mmap_buffer_t phxfs_mbuffer;
    rcu_read_lock();
        phxfs_mbuffer = phxfs_mbuffer_get_unlocked(base_index);
    rcu_read_unlock();
    return phxfs_mbuffer;
}

int phxfs_add_phony_buffer(struct file *filp, struct vm_area_struct *vma) {
    u64 buffer_len;
    int ret = -EINVAL, tries = 10;
    unsigned long base_index;
    struct phxfs_dev *dev;
    phxfs_mmap_buffer_t phxfs_mbuffer, phxfs_new_mbuffer;

    if (WARN_ON(!filp || !vma))
        return -EINVAL;

    buffer_len = vma->vm_end - vma->vm_start;

    dev = (struct phxfs_dev *)filp->private_data;
    if (dev == NULL)
        goto error;

    // if the length is smaller than 2M, check for 2M alignment
    if (buffer_len < GPU_PAGE_SIZE && (buffer_len % GPU_PAGE_SIZE)) {
        // printk("mmap size not a multiple of 64k: 0x%llx for size >64k \n", buffer_len);
    }

    phxfs_new_mbuffer = (phxfs_mmap_buffer_t)kzalloc(sizeof(struct phxfs_mmap_buffer), GFP_KERNEL);
    if (!phxfs_new_mbuffer) {
        ret = -ENOMEM;
        goto error;
    }

    spin_lock(&lock);
    tries = 10;
    do {
        base_index = PHXFS_MIN_BASE_INDEX + atomic_inc_return(&base_index_cnt);
        phxfs_new_mbuffer->base_index = base_index;
        atomic_set(&phxfs_new_mbuffer->ref, 1);
        hash_add_rcu(phxfs_io_mbuffer_hash, &phxfs_new_mbuffer->hash_link, base_index);
        phxfs_mbuffer = phxfs_new_mbuffer;
        phxfs_new_mbuffer = NULL;
        break;
        // }
    } while (tries);
    spin_unlock(&lock);

    if (phxfs_new_mbuffer != NULL) {
        kfree(phxfs_new_mbuffer);
        ret = -ENOMEM;
        goto error;
    }

    if (vma->vm_private_data == NULL) {
        vma->vm_private_data = (void *)phxfs_mbuffer;
    } else {
       
        phxfs_warn("vma->vm_private_data!=NULL\n");
        goto error;
    }

    phxfs_mbuffer->vma = vma;
    phxfs_mbuffer->dev = dev;
    phxfs_mbuffer->dev_id = dev->idx;
    phxfs_mbuffer->c_vaddr = vma->vm_start;
    phxfs_mbuffer->map_len = buffer_len;
    phxfs_mbuffer->remap = 0;

    return 0;

error:
    return ret;
}

int phxfs_mmap(struct file *filp, struct vm_area_struct *vma) {
    int ret;
    struct mm_struct *mm = current->mm;
#ifdef NVFS_VM_FLAGS_NOT_CONSTANT
    vma->vm_flags &= ~VM_PFNMAP;
    vma->vm_flags &= ~VM_IO;
    vma->vm_flags |= VM_MIXEDMAP;
    vma->vm_flags |= mm->def_flags;
#else
    unsigned long vm_flags;
    vm_flags = ACCESS_PRIVATE(vma, __vm_flags);
    vm_flags &= ~VM_PFNMAP;
    vm_flags &= ~VM_IO;
    vm_flags |= VM_MIXEDMAP;
    vm_flags |= mm->def_flags;
    vm_flags_set(vma, vm_flags);
#endif
    vma->vm_pgoff = 0;
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    if (vma->vm_pgoff == 0) {
        ret = phxfs_add_phony_buffer(filp, vma);
        return ret;
    }

    return -EINVAL;
}