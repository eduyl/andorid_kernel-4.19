/*
 * Contiguous Memory Allocator for DMA mapping framework
 * Copyright (c) 2010-2011 by Samsung Electronics.
 * Written by:
 *	Marek Szyprowski <m.szyprowski@samsung.com>
 *	Michal Nazarewicz <mina86@mina86.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License or (at your optional) any later version of the license.
 */

#define pr_fmt(fmt) "cma: " fmt

#ifdef CONFIG_CMA_DEBUG
#ifndef DEBUG
#  define DEBUG
#endif
#endif

#include <asm/page.h>
#include <asm/dma-contiguous.h>

#include <linux/memblock.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/page-isolation.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/mm_types.h>
#include <linux/dma-contiguous.h>

struct cma {
	unsigned long	base_pfn;
	unsigned long	count;
	unsigned long	free_count;
	unsigned long	*bitmap;
	unsigned long	carved_out_count;
	bool isolated;
};

// struct cma *dma_contiguous_default_area;

#ifdef CONFIG_CMA_SIZE_MBYTES
#define CMA_SIZE_MBYTES CONFIG_CMA_SIZE_MBYTES
#else
#define CMA_SIZE_MBYTES 0
#endif

/*
 * Default global CMA area size can be defined in kernel's .config.
 * This is usefull mainly for distro maintainers to create a kernel
 * that works correctly for most supported systems.
 * The size can be set in bytes or as a percentage of the total memory
 * in the system.
 *
 * Users, who want to set the size of global CMA area for their system
 * should use cma= kernel parameter.
 */
static const phys_addr_t size_bytes = CMA_SIZE_MBYTES * SZ_1M;
static phys_addr_t size_cmdline = -1;

static int __init early_cma(char *p)
{
	pr_debug("%s(%s)\n", __func__, p);
	size_cmdline = memparse(p, &p);
	return 0;
}
early_param("cma", early_cma);

#ifdef CONFIG_CMA_SIZE_PERCENTAGE

static phys_addr_t __init __maybe_unused cma_early_percent_memory(void)
{
	struct memblock_region *reg;
	unsigned long total_pages = 0;

	/*
	 * We cannot use memblock_phys_mem_size() here, because
	 * memblock_analyze() has not been called yet.
	 */
	for_each_memblock(memory, reg)
		total_pages += memblock_region_memory_end_pfn(reg) -
			       memblock_region_memory_base_pfn(reg);

	return (total_pages * CONFIG_CMA_SIZE_PERCENTAGE / 100) << PAGE_SHIFT;
}

#else

static inline __maybe_unused phys_addr_t cma_early_percent_memory(void)
{
	return 0;
}

#endif

/**
 * dma_contiguous_reserve() - reserve area for contiguous memory handling
 * @limit: End address of the reserved memory (optional, 0 for any).
 *
 * This function reserves memory from early allocator. It should be
 * called by arch specific code once the early allocator (memblock or bootmem)
 * has been activated and all other subsystems have already allocated/reserved
 * memory.
 */
// void __init dma_contiguous_reserve(phys_addr_t limit)
// {
// 	phys_addr_t selected_size = 0;

// 	pr_debug("%s(limit %08lx)\n", __func__, (unsigned long)limit);

// 	if (size_cmdline != -1) {
// 		selected_size = size_cmdline;
// 	} else {
// #ifdef CONFIG_CMA_SIZE_SEL_MBYTES
// 		selected_size = size_bytes;
// #elif defined(CONFIG_CMA_SIZE_SEL_PERCENTAGE)
// 		selected_size = cma_early_percent_memory();
// #elif defined(CONFIG_CMA_SIZE_SEL_MIN)
// 		selected_size = min(size_bytes, cma_early_percent_memory());
// #elif defined(CONFIG_CMA_SIZE_SEL_MAX)
// 		selected_size = max(size_bytes, cma_early_percent_memory());
// #endif
// 	}

// 	if (selected_size) {
// 		pr_debug("%s: reserving %ld MiB for global area\n", __func__,
// 			 (unsigned long)selected_size / SZ_1M);

// 		dma_declare_contiguous(NULL, selected_size, 0, limit);
// 	}
// };

static DEFINE_MUTEX(cma_mutex);

#ifndef CMA_NO_MIGRATION
static __init int cma_activate_area(unsigned long base_pfn, unsigned long count)
{
	unsigned long pfn = base_pfn;
	unsigned i = count >> pageblock_order;
	struct zone *zone;

	WARN_ON_ONCE(!pfn_valid(pfn));
	zone = page_zone(pfn_to_page(pfn));

	do {
		unsigned j;
		base_pfn = pfn;
		for (j = pageblock_nr_pages; j; --j, pfn++) {
			WARN_ON_ONCE(!pfn_valid(pfn));
			if (page_zone(pfn_to_page(pfn)) != zone)
				return -EINVAL;
		}
		init_cma_reserved_pageblock(pfn_to_page(base_pfn));
	} while (--i);
	return 0;
}
#else
static __init int cma_activate_area(unsigned long base_pfn, unsigned long count)
{
	return 0;
}
#endif

static __init struct cma *cma_create_area(unsigned long base_pfn,
				     unsigned long carved_out_count,
				     unsigned long count)
{
	int bitmap_size = BITS_TO_LONGS(count) * sizeof(long);
	struct cma *cma;
	int ret = -ENOMEM;

	pr_debug("%s(base %08lx, count %lx)\n", __func__, base_pfn, count);

	cma = kzalloc(sizeof *cma, GFP_KERNEL);
	if (!cma)
		return ERR_PTR(-ENOMEM);

	cma->base_pfn = base_pfn;
	cma->count = count;
	cma->free_count = count;
	cma->bitmap = kzalloc(bitmap_size, GFP_KERNEL);
#ifdef CMA_NO_MIGRATION
	cma->isolated = true;
#endif

	if (!cma->bitmap)
		goto no_mem;

	ret = cma_activate_area(base_pfn, carved_out_count);
	if (ret)
		goto error;

	pr_debug("%s: returned %p\n", __func__, (void *)cma);
	return cma;

error:
	kfree(cma->bitmap);
no_mem:
	kfree(cma);
	return ERR_PTR(ret);
}

static struct cma_reserved {
	phys_addr_t start;
	unsigned long size;
	phys_addr_t carved_out_start;
	unsigned long carved_out_size;
	struct device *dev;
} cma_reserved[MAX_CMA_AREAS] __initdata;
static unsigned cma_reserved_count __initdata;

static int __init cma_init_reserved_areas(void)
{
	struct cma_reserved *r = cma_reserved;
	unsigned i = cma_reserved_count;

	pr_debug("%s()\n", __func__);

	for (; i; --i, ++r) {
		struct cma *cma;
		cma = cma_create_area(PFN_DOWN(r->carved_out_start),
				      r->carved_out_size >> PAGE_SHIFT,
				      r->size >> PAGE_SHIFT);
		if (!IS_ERR(cma))
			dev_set_cma_area(r->dev, cma);
	}
	return 0;
}
core_initcall(cma_init_reserved_areas);

/**
 * dma_declare_contiguous() - reserve area for contiguous memory handling
 *			      for particular device
 * @dev:   Pointer to device structure.
 * @size:  Size of the reserved memory.
 * @base:  Start address of the reserved memory (optional, 0 for any).
 * @limit: End address of the reserved memory (optional, 0 for any).
 *
 * This function reserves memory for specified device. It should be
 * called by board specific code when early allocator (memblock or bootmem)
 * is still activate.
 */
// int __init dma_declare_contiguous(struct device *dev, phys_addr_t size,
// 				  phys_addr_t base, phys_addr_t limit)
// {
// 	struct cma_reserved *r = &cma_reserved[cma_reserved_count];
// 	phys_addr_t alignment;

// 	pr_debug("%s(size %lx, base %08lx, limit %08lx)\n", __func__,
// 		 (unsigned long)size, (unsigned long)base,
// 		 (unsigned long)limit);

// 	/* Sanity checks */
// 	if (cma_reserved_count == ARRAY_SIZE(cma_reserved)) {
// 		pr_err("Not enough slots for CMA reserved regions!\n");
// 		return -ENOSPC;
// 	}

// 	if (!size)
// 		return -EINVAL;

// 	r->size = PAGE_ALIGN(size);

// 	/* Sanitise input arguments */
// #ifndef CMA_NO_MIGRATION
// 	alignment = PAGE_SIZE << max(MAX_ORDER - 1, pageblock_order);
// #else
// 	/* constraints for memory protection */
// 	alignment = (size < SZ_1M) ? (SZ_4K << get_order(size)): SZ_1M;
// #endif
// 	if (base & (alignment - 1)) {
// 		pr_err("Invalid alignment of base address %pa\n", &base);
// 		return -EINVAL;
// 	}
// 	base = ALIGN(base, alignment);
// 	size = ALIGN(size, alignment);
// 	limit &= ~(alignment - 1);

// 	/* Reserve memory */
// 	if (base) {
// 		if (memblock_is_region_reserved(base, size) ||
// 		    memblock_reserve(base, size) < 0) {
// 			base = -EBUSY;
// 			goto err;
// 		}
// 	} else {
// 		/*
// 		 * Use __memblock_alloc_base() since
// 		 * memblock_alloc_base() panic()s.
// 		 */
// 		phys_addr_t addr = __memblock_alloc_base(size, alignment, limit);
// 		if (!addr) {
// 			base = -ENOMEM;
// 			goto err;
// 		} else {
// 			base = addr;
// 		}
// 	}

// 	/*
// 	 * Each reserved area must be initialised later, when more kernel
// 	 * subsystems (like slab allocator) are available.
// 	 */
// 	r->carved_out_start = base;
// 	r->carved_out_size = size;
// 	r->dev = dev;
// 	cma_reserved_count++;
// 	pr_info("CMA: reserved %ld MiB at %08lx\n", (unsigned long)size / SZ_1M,
// 		(unsigned long)base);

// 	/* Architecture specific contiguous memory fixup. */
// 	dma_contiguous_early_fixup(base, size);
// 	return 0;
// err:
// 	pr_err("CMA: failed to reserve %ld MiB\n", (unsigned long)size / SZ_1M);
// 	return base;
// }

/**
 * dma_alloc_from_contiguous() - allocate pages from contiguous area
 * @dev:   Pointer to device for which the allocation is performed.
 * @count: Requested number of pages.
 * @align: Requested alignment of pages (in PAGE_SIZE order).
 *
 * This function allocates memory buffer for specified device. It uses
 * device specific contiguous memory area if available or the default
 * global one. Requires architecture specific get_dev_cma_area() helper
 * function.
 */
// struct page *dma_alloc_from_contiguous(struct device *dev, size_t count,
// 				       unsigned int order, bool no_warn)
// {
// 	unsigned long mask, pfn, pageno, start = 0;
// 	struct cma *cma = dev_get_cma_area(dev);
// 	struct page *page = NULL;
// 	int ret;
// 	gfp_t gfp_mask = GFP_KERNEL | __GFP_ZERO; // TODO: verify this mask

// 	if (no_warn)
// 		gfp_mask |= __GFP_NOWARN;

// 	if (!cma || !cma->count)
// 		return NULL;

// 	if (order > CONFIG_CMA_ALIGNMENT)
// 		order = CONFIG_CMA_ALIGNMENT;

// 	pr_debug("%s(cma %p, count %d, align %d)\n", __func__, (void *)cma,
// 		 count, order);

// 	if (!count)
// 		return NULL;

// 	mask = (1 << order) - 1;

// 	/* HACK for MFC context buffer */
// 	if (!strncmp(dev_name(dev), "ion_video", strlen("ion_video")) && (count > 8))
// 		start = 16;

// 	mutex_lock(&cma_mutex);

// 	for (;;) {
// 		pageno = bitmap_find_next_zero_area(cma->bitmap, cma->count,
// 						    start, count, mask);
// 		if (pageno >= cma->count)
// 			break;

// 		pfn = cma->base_pfn + pageno;
// 		ret = cma->isolated ?
// 			0 : alloc_contig_range(pfn, pfn + count, MIGRATE_CMA, gfp_mask);
// 		if (ret == 0) {
// 			bitmap_set(cma->bitmap, pageno, count);
// 			page = pfn_to_page(pfn);
// 			cma->free_count -= count;
// 			break;
// 		} else if (ret != -EBUSY) {
// 			break;
// 		}
// 		pr_debug("%s(): memory range at %p is busy, retrying\n",
// 			 __func__, pfn_to_page(pfn));
// 		/* try again with a bit different memory target */
// 		start = pageno + mask + 1;
// 	}

// 	mutex_unlock(&cma_mutex);
// 	pr_debug("%s(): returned %p\n", __func__, page);
// 	return page;
// }

/**
 * dma_release_from_contiguous() - release allocated pages
 * @dev:   Pointer to device for which the pages were allocated.
 * @pages: Allocated pages.
 * @count: Number of allocated pages.
 *
 * This function releases memory allocated by dma_alloc_from_contiguous().
 * It returns false when provided pages do not belong to contiguous area and
 * true otherwise.
 */
// bool dma_release_from_contiguous(struct device *dev, struct page *pages,
// 				 int count)
// {
// 	struct cma *cma = dev_get_cma_area(dev);
// 	unsigned long pfn;

// 	if (!cma || !pages)
// 		return false;

// 	pr_debug("%s(page %p)\n", __func__, (void *)pages);

// 	pfn = page_to_pfn(pages);

// 	if (pfn < cma->base_pfn || pfn >= cma->base_pfn + cma->count)
// 		return false;

// 	VM_BUG_ON(pfn + count > cma->base_pfn + cma->count);

// 	mutex_lock(&cma_mutex);
// 	bitmap_clear(cma->bitmap, pfn - cma->base_pfn, count);
// 	if (!cma->isolated)
// 		free_contig_range(pfn, count);
// 	cma->free_count += count;
// 	mutex_unlock(&cma_mutex);

// 	return true;
// }

/**
 * dma_contiguous_info() - retrieving contiguous memory information
 * @dev:  Pointer to device to get the information.
 * @info: [OUT] pointer to a structure to store the information
 *
 * This fills @info the status of a contiguous memory. -ENODEV if
 * the given device does not have contiguous memory.
 */
int dma_contiguous_info(struct device *dev, struct cma_info *info)
{
	struct cma *cma = dev_get_cma_area(dev);

	if (!info)
		return 0;

	if (!cma)
		return -ENODEV;

	info->base = cma->base_pfn << PAGE_SHIFT;
	info->size = cma->count << PAGE_SHIFT;
	info->free = cma->free_count << PAGE_SHIFT;
	info->isolated = cma->isolated;

	return 0;
}

#ifndef CMA_NO_MIGRATION
static void dma_contiguous_deisolate_until(struct device *dev, int idx_until)
{
	struct cma *cma = dev_get_cma_area(dev);
	int idx;

	if (!cma || !idx_until)
		return;

	mutex_lock(&cma_mutex);

	if (!cma->isolated) {
		mutex_unlock(&cma_mutex);
		dev_err(dev, "Not isolated!\n");
		return;
	}

	idx = find_first_zero_bit(cma->bitmap, idx_until);
	while (idx < idx_until) {
		int idx_set;

		idx_set = find_next_bit(cma->bitmap, idx_until, idx);

		free_contig_range(cma->base_pfn + idx, idx_set - idx);

		idx = find_next_zero_bit(cma->bitmap, idx_until, idx_set);
	}

	cma->isolated = false;

	mutex_unlock(&cma_mutex);
}

/**
 * dma_contiguous_deisolate() - return contiguous memory to the page allocator
 * @dev: Pointer to device which owns the contiguous memory
 *
 * This function return the contiguous memory that is not allocated by CMA to
 * the page allocator so that the kernel can allocate the contiguous memory.
 */
void dma_contiguous_deisolate(struct device *dev)
{
	struct cma *cma = dev_get_cma_area(dev);
	dma_contiguous_deisolate_until(dev, cma->count);
}

/**
 * dma_contiguous_isolate() - isolate contiguous memory from the page allocator
 * @dev: Pointer to device which owns the contiguous memory
 *
 * This function isolates contiguous memory from the page allocator. If some of
 * the contiguous memory is allocated, it is reclaimed.
 */
int dma_contiguous_isolate(struct device *dev)
{
	struct cma *cma = dev_get_cma_area(dev);
	int ret;
	int idx;

	if (!cma)
		return -ENODEV;

	if (cma->count == 0)
		return 0;

	mutex_lock(&cma_mutex);

	if (cma->isolated) {
		mutex_unlock(&cma_mutex);
		dev_err(dev, "Alread isolated!\n");
		return 0;
	}

	idx = find_first_zero_bit(cma->bitmap, cma->count);
	while (idx < cma->count) {
		int idx_set;

		idx_set = find_next_bit(cma->bitmap, cma->count, idx);
		ret = alloc_contig_range(cma->base_pfn + idx,
					cma->base_pfn + idx_set,
					MIGRATE_CMA, GFP_KERNEL | __GFP_NOWARN ); // TODO: ijh verify gfp mask
		if (ret != 0) {
			mutex_unlock(&cma_mutex);
			dma_contiguous_deisolate_until(dev, idx_set);
			dev_err(dev, "Failed to isolate %#lx@%#010x.\n",
				(idx_set - idx) * PAGE_SIZE,
				PFN_PHYS(cma->base_pfn + idx));
			return ret;
		}

		idx = find_next_zero_bit(cma->bitmap, cma->count, idx_set);
	}

	cma->isolated = true;

	mutex_unlock(&cma_mutex);

	return 0;
}
#endif /* CMA_NO_MIGRATION */
