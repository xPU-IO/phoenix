#include <asm/page.h>
#include <linux/cdev.h>
#include <linux/ctype.h> //for isdigit()
#include <linux/device.h>
#include <linux/fcntl.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/memory.h>
#include <linux/module.h>
#include <linux/nvme_ioctl.h>
#include <linux/pci-p2pdma.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/seq_buf.h>
#include <linux/thread_info.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include "config-host.h"
#include "phxfs-mem.h"
#include "phxfs.h"

#include "nvfs-p2p.h"
#include "nvfs-pci.h"

static DEFINE_IDA(phxfs_chr_minor_ida);
static dev_t phxfs_chr_devt;
static struct class *phxfs_chr_class;
struct device phxfs_chr_dev_device;
struct cdev phxfs_chr_dev;

#define PHXFS_MINORS 1

struct phxfs_ctrl ctrl;

#define NUM_THREADS 128
u32 npu_num;
extern uint64_t gpu_info_table[MAX_GPU_DEVS];

#define PHXFS_PAT_PATH "/sys/kernel/debug/x86/pat_memtype_list"
#define PHXFS_PAT_BUF_SIZE (64 * 1024) /* PAT file typically < 16 KiB */

/* PAT conflict range recorded during parsing */
struct phxfs_pat_conflict {
	u64 start;
	u64 end;
};

/*
 * Read PAT memtype list and extract conflict ranges that overlap
 * with [bar_start, bar_start + bar_len).
 * Returns number of conflicts found, or negative errno.
 * conflicts array is allocated by caller with max_entries capacity.
 */
static int phxfs_read_pat_conflicts(u64 bar_start, u64 bar_len,
				    struct phxfs_pat_conflict *conflicts,
				    int max_entries)
{
	struct file *filp;
	loff_t pos = 0;
	char *buf;
	int ret, n_conflicts = 0;
	u64 bar_end = bar_start + bar_len;

	buf = kzalloc(PHXFS_PAT_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	filp = filp_open(PHXFS_PAT_PATH, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		printk(KERN_WARNING "phxfs: cannot open %s (err=%ld), "
		       "skipping PAT conflict detection\n",
		       PHXFS_PAT_PATH, PTR_ERR(filp));
		kfree(buf);
		return 0; /* graceful: no conflicts detected */
	}

	ret = kernel_read(filp, buf, PHXFS_PAT_BUF_SIZE - 1, &pos);
	filp_close(filp, NULL);

	if (ret <= 0) {
		printk(KERN_WARNING "phxfs: failed to read %s (ret=%d)\n",
		       PHXFS_PAT_PATH, ret);
		kfree(buf);
		return 0;
	}
	buf[ret] = '\0';

	/* Parse lines like: "write-combining @ 0x21a000800000-0x21a000900000" */
	{
		char *line = buf;
		while (line && *line) {
			char *nl = strchr(line, '\n');
			u64 cs, ce;
			char memtype[64] = {0};

			if (nl)
				*nl = '\0';

			/* Skip write-back entries - they don't conflict with cached mapping */
			if (strncmp(line, "write-back", 10) == 0 ||
			    strncmp(line, "PAT", 3) == 0) {
				line = nl ? nl + 1 : NULL;
				continue;
			}

			/* Try to parse: <memtype> @ 0x<start>-0x<end> */
			if (sscanf(line, "%63s @ 0x%llx-0x%llx", memtype, &cs, &ce) == 3) {
				/* Check if this range overlaps with our BAR window */
				if (cs < bar_end && ce > bar_start) {
					/* Clip to BAR range */
					u64 clipped_start = max(cs, bar_start);
					u64 clipped_end = min(ce, bar_end);

					if (n_conflicts < max_entries) {
						conflicts[n_conflicts].start = clipped_start;
						conflicts[n_conflicts].end = clipped_end;
						n_conflicts++;
					} else {
						printk(KERN_WARNING "phxfs: too many PAT conflicts "
						       "(>%d), some skipped\n", max_entries);
						break;
					}
				}
			}
			line = nl ? nl + 1 : NULL;
		}
	}

	kfree(buf);
	return n_conflicts;
}

/*
 * Compute which REMAP_UNIT_SIZE-aligned blocks within the HBM region are free of
 * PAT conflicts, then merge adjacent free blocks into segments.
 *
 * Strategy:
 *   - Reserve [paddr, paddr + PHXFS_RESERVED_SIZE) at the head
 *   - Reserve [paddr + hbm_size - PHXFS_RESERVED_SIZE, paddr + hbm_size) at the tail
 *   - Divide the middle region into REMAP_UNIT_SIZE blocks
 *   - Mark blocks that overlap any PAT conflict as "skip"
 *   - Merge consecutive non-skip blocks into segments
 *
 * Returns number of segments, or negative errno.
 * Caller must kfree(*out_segments) when done.
 */
static int phxfs_compute_bar_segments(
	u64 paddr, u64 hbm_size,
	struct phxfs_pat_conflict *conflicts, int n_conflicts,
	struct phxfs_bar_segment **out_segments)
{
	u64 usable_start, usable_end, region_size;
	int n_blocks, i, s;
	int n_segments = 0;
	bool *block_skip; /* true = has PAT conflict, skip this block */
	struct phxfs_bar_segment *segs;

	usable_start = paddr + PHXFS_RESERVED_SIZE;
	usable_end = paddr + hbm_size - PHXFS_RESERVED_SIZE;

	if (usable_end <= usable_start) {
		printk(KERN_WARNING "phxfs: HBM too small for head/tail reservation "
		       "(hbm_size=%llu MiB)\n", hbm_size / (1024 * 1024));
		return -ENOSPC;
	}

	region_size = usable_end - usable_start;
	n_blocks = (int)(region_size / PHXFS_REMAP_UNIT_SIZE);
	if (n_blocks <= 0)
		return -ENOSPC;

	block_skip = kcalloc(n_blocks, sizeof(bool), GFP_KERNEL);
	if (!block_skip)
		return -ENOMEM;

	/* Mark blocks that overlap with any PAT conflict */
	for (i = 0; i < n_blocks; i++) {
		u64 blk_start = usable_start + (u64)i * PHXFS_REMAP_UNIT_SIZE;
		u64 blk_end = blk_start + PHXFS_REMAP_UNIT_SIZE;
		int c;

		for (c = 0; c < n_conflicts; c++) {
			if (conflicts[c].start < blk_end && conflicts[c].end > blk_start) {
				block_skip[i] = true;
				break;
			}
		}
	}

	/* Count how many merged segments we need */
	for (i = 0; i < n_blocks; ) {
		if (!block_skip[i]) {
			/* Start of a run of non-skip blocks */
			while (i < n_blocks && !block_skip[i])
				i++;
			n_segments++;
		} else {
			i++;
		}
	}

	if (n_segments == 0) {
		printk(KERN_WARNING "phxfs: no remappable segments found\n");
		kfree(block_skip);
		return -ENOSPC;
	}

	segs = kcalloc(n_segments, sizeof(struct phxfs_bar_segment), GFP_KERNEL);
	if (!segs) {
		kfree(block_skip);
		return -ENOMEM;
	}

	/* Build merged segments */
	s = 0;
	for (i = 0; i < n_blocks && s < n_segments; ) {
		if (!block_skip[i]) {
			u64 seg_start = usable_start + (u64)i * PHXFS_REMAP_UNIT_SIZE;
			int run_len = 0;

			while (i < n_blocks && !block_skip[i]) {
				run_len++;
				i++;
			}

			segs[s].phys_start = seg_start;
			segs[s].size = (u64)run_len * PHXFS_REMAP_UNIT_SIZE;
			segs[s].va = NULL;
			segs[s].p2p_pgmap = NULL;
			s++;
		} else {
			i++;
		}
	}

	kfree(block_skip);
	*out_segments = segs;
	return s;
}

int extract_trailing_number(const char str[]) {
	int number = 0;
	int multiplier = 1;
	size_t len;
	int found_digit = 0;
	int i;
	len = strlen(str);

	for (i = len - 1; i >= 0; --i) {
		if (isdigit(str[i])) {
			number += (str[i] - '0') * multiplier;
			found_digit = 1;
			if (multiplier == 1) {
				multiplier = 10;
			} else if (found_digit) {
				break;
			}
		} else if (found_digit) {
			break;
		}
	}

	if (found_digit) {
		return number;
	} else {
		return -1;
	}
}

static int phxfs_devm_memremap(struct phxfs_dev *phx_dev) {
	struct phxfs_pat_conflict *conflicts = NULL;
	struct phxfs_bar_segment *segs = NULL;
	int n_conflicts, n_segments;
	int i, ret = 1;

	printk(KERN_INFO "phxfs%d: BAR size=%llu MiB, paddr=0x%llx\n",
	       phx_dev->idx, phx_dev->size / (1024 * 1024), phx_dev->paddr);

	/* Max blocks = usable region / unit size, used as upper bound for conflicts */
	{
		int max_blocks = (int)((phx_dev->size - 2 * PHXFS_RESERVED_SIZE) / PHXFS_REMAP_UNIT_SIZE);
		if (max_blocks <= 0) {
			printk(KERN_WARNING "phxfs%d: BAR too small for head/tail reservation\n",
			       phx_dev->idx);
			return -ENOSPC;
		}

		/* Detect PAT conflicts within [paddr, paddr + size) */
		conflicts = kcalloc(max_blocks, sizeof(struct phxfs_pat_conflict), GFP_KERNEL);
		if (!conflicts)
			return -ENOMEM;

		n_conflicts = phxfs_read_pat_conflicts(phx_dev->paddr, phx_dev->size,
						       conflicts, max_blocks);
	}
	if (n_conflicts < 0) {
		printk(KERN_WARNING "phxfs%d: PAT conflict detection failed (%d), "
		       "falling back to full BAR remap\n", phx_dev->idx, n_conflicts);
		kfree(conflicts);
		/* Fallback: remap entire BAR as single segment */
		goto fallback_single;
	}

	printk(KERN_INFO "phxfs%d: found %d PAT conflict(s) in BAR region\n",
	       phx_dev->idx, n_conflicts);

	for (i = 0; i < n_conflicts; i++) {
		u64 off_start = conflicts[i].start - phx_dev->paddr;
		u64 off_end = conflicts[i].end - phx_dev->paddr;
		printk(KERN_INFO "phxfs%d:   conflict %d: offset 0x%llx-0x%llx "
		       "(%llu MiB - %llu MiB)\n",
		       phx_dev->idx, i, off_start, off_end,
		       off_start / (1024 * 1024), off_end / (1024 * 1024));
	}

	/* Compute segments (skip conflicts, merge adjacent free blocks) */
	n_segments = phxfs_compute_bar_segments(phx_dev->paddr, phx_dev->size,
						conflicts, n_conflicts, &segs);
	kfree(conflicts);

	if (n_segments <= 0) {
		printk(KERN_WARNING "phxfs%d: no valid segments computed, "
		       "falling back to full BAR remap\n", phx_dev->idx);
		goto fallback_single;
	}

	printk(KERN_INFO "phxfs%d: computed %d segment(s) after conflict skipping + merging\n",
	       phx_dev->idx, n_segments);
	for (i = 0; i < n_segments; i++) {
		u64 off = segs[i].phys_start - phx_dev->paddr;
		printk(KERN_INFO "phxfs%d:   segment %d: offset 0x%llx, size %llu MiB\n",
		       phx_dev->idx, i, off, segs[i].size / (1024 * 1024));
	}

	/* Perform devm_memremap_pages for each segment */
	phx_dev->segments = segs;
	phx_dev->num_segments = 0;

	for (i = 0; i < n_segments; i++) {
		struct dev_pagemap *pgmap;
		struct pci_p2pdma_pagemap *p2p_pgmap;

		p2p_pgmap = devm_kzalloc(&phx_dev->dev->dev,
					  sizeof(struct pci_p2pdma_pagemap), GFP_KERNEL);
		if (!p2p_pgmap) {
			ret = -ENOMEM;
			goto err_cleanup;
		}

		pgmap = &p2p_pgmap->pgmap;
		pgmap->res.start = segs[i].phys_start;
		pgmap->res.end = segs[i].phys_start + segs[i].size - 1;
		pgmap->res.flags = IORESOURCE_MEM;
		pgmap->type = MEMORY_DEVICE_PCI_P2PDMA;

		segs[i].va = devm_memremap_pages(&phx_dev->dev->dev, pgmap);
		if (IS_ERR_OR_NULL(segs[i].va)) {
			printk(KERN_WARNING "phxfs%d: devm_memremap_pages failed for "
			       "segment %d (offset 0x%llx, size %llu MiB), err=%ld\n",
			       phx_dev->idx, i,
			       segs[i].phys_start - phx_dev->paddr,
			       segs[i].size / (1024 * 1024),
			       PTR_ERR(segs[i].va));
			segs[i].va = NULL;
			devm_kfree(&phx_dev->dev->dev, p2p_pgmap);
			/* Continue with remaining segments */
			continue;
		}

		segs[i].p2p_pgmap = p2p_pgmap;
		phx_dev->num_segments++;
		printk(KERN_INFO "phxfs%d: segment %d remapped: offset 0x%llx, "
		       "size %llu MiB, va=0x%lx\n",
		       phx_dev->idx, i,
		       segs[i].phys_start - phx_dev->paddr,
		       segs[i].size / (1024 * 1024),
		       (unsigned long)segs[i].va);
	}

	if (phx_dev->num_segments == 0) {
		printk(KERN_ERR "phxfs%d: all segment remaps failed\n", phx_dev->idx);
		ret = -ENOMEM;
		goto err_cleanup;
	}

	/* For compatibility: set pci_mem_va to first segment's VA */
	phx_dev->pci_mem_va = segs[0].va;
	phx_dev->remap = 1;

	/* Also set legacy p2p_pgmap to first segment's for any old cleanup paths */
	phx_dev->p2p_pgmap = segs[0].p2p_pgmap;

	/* Calculate total remapped size */
	{
		u64 total_remapped = 0;
		for (i = 0; i < phx_dev->num_segments; i++)
			total_remapped += segs[i].size;
		printk(KERN_INFO "phxfs%d: successfully remapped %d segments, "
		       "total %llu MiB out of %llu MiB HBM\n",
		       phx_dev->idx, phx_dev->num_segments,
		       total_remapped / (1024 * 1024),
		       phx_dev->size / (1024 * 1024));
	}

	return 0;

fallback_single:
	/* Legacy single-segment full BAR remap */
	{
	struct dev_pagemap *pgmap;

	phx_dev->p2p_pgmap = devm_kzalloc(&phx_dev->dev->dev,
									sizeof(struct pci_p2pdma_pagemap), GFP_KERNEL);
		if (!phx_dev->p2p_pgmap)
		return -ENOMEM;

	printk("npu_devm_memremap 1\n");
	pgmap = &phx_dev->p2p_pgmap->pgmap;

	// pgmap->range.start = phx_dev->paddr;
	// pgmap->range.end = phx_dev->paddr + phx_dev->size - 1;
	// printk("npu->pgmap->res.start is %llx, end is %llx\n", pgmap->range.start,
	// 		pgmap->range.end);
	// pgmap->nr_range = 1;
	// pgmap->type = MEMORY_DEVICE_PCI_P2PDMA;

	phx_dev->pgmap_res.start = phx_dev->paddr;
	phx_dev->pgmap_res.end = phx_dev->paddr + phx_dev->size - 1;		
	phx_dev->pgmap_res.flags = IORESOURCE_MEM;

	printk("npu->pgmap->res.start is %llx, end is %llx\n",
    	phx_dev->pgmap_res.start,
       	phx_dev->pgmap_res.end);

	pgmap->res = phx_dev->pgmap_res;
	pgmap->type = MEMORY_DEVICE_PCI_P2PDMA;

	phx_dev->pci_mem_va = devm_memremap_pages(&phx_dev->dev->dev, pgmap);

	printk("npu numa is %d\n", phx_dev->dev->dev.numa_node);

	if (IS_ERR_OR_NULL(phx_dev->pci_mem_va)) {
			printk(KERN_ERR "phxfs%d: fallback devm_memremap_pages failed\n",
			       phx_dev->idx);
		devm_kfree(&phx_dev->dev->dev, phx_dev->p2p_pgmap);
			return -ENOMEM;
	}

	printk("npu devm_memremap_pages success, addr is %lx\n",
			(uintptr_t)phx_dev->pci_mem_va);
	phx_dev->remap = 1;
		phx_dev->segments = NULL;
		phx_dev->num_segments = 0;

		printk(KERN_INFO "phxfs%d: fallback single-segment remap, va=0x%lx\n",
		       phx_dev->idx, (unsigned long)phx_dev->pci_mem_va);
		return 0;
	}

err_cleanup:
	/* Clean up any segments that were successfully remapped */
	for (i = 0; i < n_segments; i++) {
		if (segs[i].p2p_pgmap && segs[i].va) {
			devm_memunmap_pages(&phx_dev->dev->dev, &segs[i].p2p_pgmap->pgmap);
		}
		if (segs[i].p2p_pgmap) {
			devm_kfree(&phx_dev->dev->dev, segs[i].p2p_pgmap);
		}
	}
	kfree(segs);
	phx_dev->segments = NULL;
	phx_dev->num_segments = 0;
	return ret;
}

static int phxfs_ctrl_init(struct phxfs_ctrl *dev_ctrl, u32 dev_num) {
	int i, j, ret;
	u64 size;
	u16 bus, fn;
	dev_ctrl->dev_num = dev_num;
	for (i = 0; i < dev_ctrl->dev_num; i++) {
		bus = (gpu_info_table[i] >> 8) & 0xFF;
		fn = gpu_info_table[i] & 0xFF;
		dev_ctrl->phx_dev[i].dev = pci_get_domain_bus_and_slot(0, bus, fn);
		if (dev_ctrl->phx_dev[i].dev == NULL) {
			printk("npu%u: pci_get_domain_bus_and_slot failed\n", i);
			return -1;
		}
		// for (j = 0; j < PCI_STD_NUM_BARS; j++) {
		for (j = 0; j <= PCI_STD_RESOURCE_END; j++) {
			size = pci_resource_len(dev_ctrl->phx_dev[i].dev, j);
			if (size > dev_ctrl->phx_dev[i].size){
				dev_ctrl->phx_dev[i].paddr = pci_resource_start(dev_ctrl->phx_dev[i].dev, j);
				dev_ctrl->phx_dev[i].size = size;
			}
		}
		dev_ctrl->phx_dev[i].idx = i;
		dev_ctrl->phx_dev[i].remap = 0;
		dev_ctrl->phx_dev[i].segments = NULL;
		dev_ctrl->phx_dev[i].num_segments = 0;
		printk("npu%u: bus is %x, size is %llu, paddr is %llx\n", i,
			dev_ctrl->phx_dev[i].dev->bus->number, dev_ctrl->phx_dev[i].size,
			dev_ctrl->phx_dev[i].paddr);
		ret = phxfs_devm_memremap(&dev_ctrl->phx_dev[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int phxfs_open(struct inode *inode, struct file *filp) {
	int ret = 0;
	int dev_idx;
	char *file_name;
	file_name = filp->f_path.dentry->d_iname; 

	if (file_name != NULL) {
		dev_idx = extract_trailing_number(file_name);
		printk("phxfs_open %s, npu_idx is %d\n", file_name, dev_idx);
		if (dev_idx < 0 || dev_idx >= ctrl.dev_num) {
			ret = -1;
			goto out;
		}
		filp->private_data = &ctrl.phx_dev[dev_idx];
	}
out:
	printk("phxfs_open %d\n", ret);
	return ret;
}

static int phxfs_release(struct inode *inode, struct file *filp) { return 0; }

static long phxfs_ioctl(struct file *filp, unsigned int cmd,
                        unsigned long arg) {
	void __user *argp = (void *)arg;
	switch (cmd) {
		case PHXFS_IOCTL_MAP: {
			struct phxfs_ioctl_map_s map_param;
			if (copy_from_user(&map_param, argp, sizeof(struct phxfs_ioctl_map_s)))
				return -EFAULT;
			return phxfs_map_dev_addr(&map_param, map_param.n_vaddr, map_param.n_size,
									map_param.c_vaddr, map_param.c_size);
		}
		case PHXFS_IOCTL_UNMAP: {
			struct phxfs_ioctl_map_s map_param;
			if (copy_from_user(&map_param, argp, sizeof(struct phxfs_ioctl_map_s)))
				return -EFAULT;
			phxfs_map_dev_release(&map_param, map_param.n_vaddr, map_param.n_size,
								map_param.c_vaddr, map_param.c_size);
			return 0;
		}
		default:
			return -ENOTTY;
	}
}

static const struct file_operations phxfs_chr_fops = {
    .owner = THIS_MODULE,
    .open = phxfs_open,
    .release = phxfs_release,
    .unlocked_ioctl = phxfs_ioctl,
    .mmap = phxfs_mmap,
};

void phxfs_cdev_del(struct cdev *cdev, struct device *cdev_device,
                    struct phxfs_dev *dev) {
	cdev_device_del(cdev, cdev_device);
	if (dev->remap) {
		if (dev->segments && dev->num_segments > 0) {
			/* Multi-segment cleanup */
			int i;
			for (i = 0; i < dev->num_segments; i++) {
				if (dev->segments[i].p2p_pgmap && dev->segments[i].va) {
					devm_memunmap_pages(&dev->dev->dev,
							    &dev->segments[i].p2p_pgmap->pgmap);
				}
				if (dev->segments[i].p2p_pgmap) {
					devm_kfree(&dev->dev->dev, dev->segments[i].p2p_pgmap);
				}
			}
			kfree(dev->segments);
			dev->segments = NULL;
			dev->num_segments = 0;
		} else if (dev->p2p_pgmap) {
			/* Legacy single-segment cleanup */
		devm_memunmap_pages(&dev->dev->dev, &dev->p2p_pgmap->pgmap);
		}
		dev->pci_mem_va = NULL;
	}
	if (dev->p2p_pgmap != NULL && !(dev->segments && dev->num_segments > 0)) {
		devm_kfree(&dev->dev->dev, dev->p2p_pgmap);
	}
	dev->dev = NULL;
	ida_simple_remove(&phxfs_chr_minor_ida, dev->idx);
}

int phxfs_cdev_add(struct cdev *cdev, struct device *cdev_device,
                   const struct file_operations *fops, struct module *owner,
                   struct phxfs_dev *dev) {
	int ret;
	ret = ida_simple_get(&phxfs_chr_minor_ida, 0, MAX_DEV_NUM, GFP_KERNEL);
	if (ret < 0)
		return ret;
	dev->idx = ret;
	ret = dev_set_name(cdev_device, "phxfs_dev%d", dev->idx);
	if (ret) {
		ida_simple_remove(&phxfs_chr_minor_ida, dev->idx);
		return ret;
	}
	cdev_device->devt = MKDEV(MAJOR(phxfs_chr_devt), dev->idx);
	cdev_device->class = phxfs_chr_class;
	device_initialize(cdev_device);
	cdev_init(cdev, fops);
	cdev->owner = owner;
	ret = cdev_device_add(cdev, cdev_device);
	if (ret) {
		ida_simple_remove(&phxfs_chr_minor_ida, dev->idx);
		return ret;
	}
	dev_set_drvdata(cdev_device, dev);
	ret = device_create_file(cdev_device, &dev_attr_pci_bdf);
	if (ret) {
		cdev_device_del(cdev, cdev_device);
		ida_simple_remove(&phxfs_chr_minor_ida, dev->idx);
	}
	return ret;
}

int phxfs_cdev_init(struct phxfs_ctrl *ctrl) {
	int ret = -ENOMEM;
	int i;
	ret = alloc_chrdev_region(&phxfs_chr_devt, 0, ctrl->dev_num,
								"phxfs-generic");
	if (ret < 0)
		goto destroy_subsys_class;
#ifdef CLASS_CREATE_HAS_TWO_PARAMS
  	phxfs_chr_class = class_create(THIS_MODULE, "phxfs-generic");
#else
  	phxfs_chr_class = class_create("phxfs-generic");
#endif
	if (IS_ERR(phxfs_chr_class)) {
		ret = PTR_ERR(phxfs_chr_class);
		goto unregister_generic_phxfs;
	}
	for (i = 0; i < ctrl->dev_num; i++) {
		ret = phxfs_cdev_add(&ctrl->phx_dev[i].cdev, &ctrl->phx_dev[i].device,
							&phxfs_chr_fops, THIS_MODULE, &ctrl->phx_dev[i]);
		if (ret) {
		kfree_const(ctrl->phx_dev[i].device.kobj.name);
		goto unregister_generic_phxfs;
		}
	}
	printk("phxfs_cdev_init success!\n");
	return 0;

unregister_generic_phxfs:
  	unregister_chrdev_region(phxfs_chr_devt, ctrl->dev_num);

destroy_subsys_class:
	class_destroy(phxfs_chr_class);
	return ret;
}

static int __init phxfs_init(void) {
	int ret, i;

	if (nvfs_nvidia_p2p_init()) {
		printk("Could not load nvidia_p2p* symbols\n");
		ret = -EOPNOTSUPP;
		return -1;
	}

	nvfs_fill_gpu2peer_distance_table_once();
	npu_num = 0;
	for (i = 0; i < MAX_DEV_NUM; i++) {
		if (gpu_info_table[i] != 0) {
			npu_num++;
		} else {
			break;
		}
	}

	printk("devdrv_get_devnum num:%d\n", npu_num);

	if (npu_num <= 0 || npu_num > MAX_DEV_NUM) {
		printk("devdrv_get_devnum error:%u\n", npu_num);
		return -1;
	}
	ret = phxfs_ctrl_init(&ctrl, npu_num);
	if (ret != 0) {
		printk("npu_ctrl_init error:%d\n", ret);
		return -1;
	}
	ret = phxfs_cdev_init(&ctrl);
	if (ret) {
		printk("phxfs_init error!\n");
		return -1;
	}
	phxfs_mbuffer_init();
	return 0;
}

static void __exit phxfs_exit(void) {
	int i;
	for (i = 0; i < ctrl.dev_num; i++) {
		phxfs_cdev_del(&ctrl.phx_dev[i].cdev, &ctrl.phx_dev[i].device, &ctrl.phx_dev[i]);
	}

	nvfs_nvidia_p2p_exit();

	class_destroy(phxfs_chr_class);
	unregister_chrdev_region(phxfs_chr_devt, PHXFS_MINORS);
	ida_destroy(&phxfs_chr_minor_ida);

	printk("Good bye!");
}

module_init(phxfs_init);
module_exit(phxfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("qiushi <qiushijsxs@outlook.com>");
MODULE_DESCRIPTION("NPU/NVIDIA direct storgae");
MODULE_VERSION("0.0.1");