/*
 *
 * (C) COPYRIGHT 2010-2017 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 */



/*
 * Base kernel context APIs
 */

#include <mali_kbase.h>
#include <mali_midg_regmap.h>
#include <mali_kbase_mem_linux.h>
#include <mali_kbase_dma_fence.h>
#include <mali_kbase_ctx_sched.h>

/**
 * kbase_create_context() - Create a kernel base context.
 * @kbdev: Kbase device
 * @is_compat: Force creation of a 32-bit context
 *
 * Allocate and init a kernel base context.
 *
 * Return: new kbase context
 */
struct kbase_context *
kbase_create_context(struct kbase_device *kbdev, bool is_compat)
{
	struct kbase_context *kctx;
	int err;
	struct page *p;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	/* zero-inited as lot of code assume it's zero'ed out on create */
	kctx = vzalloc(sizeof(*kctx));

	if (!kctx)
		goto out;

	/* creating a context is considered a disjoint event */
	kbase_disjoint_event(kbdev);

	kctx->kbdev = kbdev;
	kctx->as_nr = KBASEP_AS_NR_INVALID;
	atomic_set(&kctx->refcount, 0);
	if (is_compat)
		kbase_ctx_flag_set(kctx, KCTX_COMPAT);
#if defined(CONFIG_64BIT)
	else
		kbase_ctx_flag_set(kctx, KCTX_FORCE_SAME_VA);
#endif /* !defined(CONFIG_64BIT) */

#ifdef CONFIG_MALI_TRACE_TIMELINE
	kctx->timeline.owner_tgid = task_tgid_nr(current);
#endif
	atomic_set(&kctx->setup_complete, 0);
	atomic_set(&kctx->setup_in_progress, 0);
	spin_lock_init(&kctx->mm_update_lock);
	kctx->process_mm = NULL;
	atomic_set(&kctx->nonmapped_pages, 0);
	kctx->slots_pullable = 0;
	kctx->tgid = current->tgid;
	kctx->pid = current->pid;

	err = kbase_mem_pool_init(&kctx->mem_pool,
				  kbdev->mem_pool_max_size_default,
				  KBASE_MEM_POOL_4KB_PAGE_TABLE_ORDER,
				  kctx->kbdev,
				  &kbdev->mem_pool);
	if (err)
		goto free_kctx;

	err = kbase_mem_pool_init(&kctx->lp_mem_pool,
				  (kbdev->mem_pool_max_size_default >> 9),
				  KBASE_MEM_POOL_2MB_PAGE_TABLE_ORDER,
				  kctx->kbdev,
				  &kbdev->lp_mem_pool);
	if (err)
		goto free_mem_pool;

	err = kbase_mem_evictable_init(kctx);
	if (err)
		goto free_both_pools;

	atomic_set(&kctx->used_pages, 0);

	err = kbase_jd_init(kctx);
	if (err)
		goto deinit_evictable;

	err = kbasep_js_kctx_init(kctx);
	if (err)
		goto free_jd;	/* safe to call kbasep_js_kctx_term  in this case */

	err = kbase_event_init(kctx);
	if (err)
		goto free_jd;

	atomic_set(&kctx->drain_pending, 0);

	mutex_init(&kctx->reg_lock);

	mutex_init(&kctx->mem_partials_lock);
	INIT_LIST_HEAD(&kctx->mem_partials);

	INIT_LIST_HEAD(&kctx->waiting_soft_jobs);
	spin_lock_init(&kctx->waiting_soft_jobs_lock);
	err = kbase_dma_fence_init(kctx);
	if (err)
		goto free_event;

	err = kbase_mmu_init(kctx);
	if (err)
		goto term_dma_fence;

	do {
		err = kbase_mem_pool_grow(&kctx->mem_pool,
				MIDGARD_MMU_BOTTOMLEVEL);
		if (err)
			goto pgd_no_mem;

		mutex_lock(&kctx->mmu_lock);
		kctx->pgd = kbase_mmu_alloc_pgd(kctx);
		mutex_unlock(&kctx->mmu_lock);
	} while (!kctx->pgd);

	p = kbase_mem_alloc_page(&kctx->mem_pool);
	if (!p)
		goto no_sink_page;
	kctx->aliasing_sink_page = as_tagged(page_to_phys(p));

	init_waitqueue_head(&kctx->event_queue);

	kctx->cookies = KBASE_COOKIE_MASK;

	/* Make sure page 0 is not used... */
	err = kbase_region_tracker_init(kctx);
	if (err)
		goto no_region_tracker;

	err = kbase_sticky_resource_init(kctx);
	if (err)
		goto no_sticky;

	err = kbase_jit_init(kctx);
	if (err)
		goto no_jit;
#ifdef CONFIG_GPU_TRACEPOINTS
	atomic_set(&kctx->jctx.work_id, 0);
#endif
#ifdef CONFIG_MALI_TRACE_TIMELINE
	atomic_set(&kctx->timeline.jd_atoms_in_flight, 0);
#endif

	kctx->id = atomic_add_return(1, &(kbdev->ctx_num)) - 1;

	mutex_init(&kctx->vinstr_cli_lock);

	timer_setup(&kctx->soft_job_timeout, kbasep_soft_job_timeout_worker, 0);

	/* MALI_SEC_INTEGRATION */
	if (kbdev->vendor_callbacks->create_context)
		kbdev->vendor_callbacks->create_context(kctx);

	/* MALI_SEC_INTEGRATION */
	atomic_set(&kctx->mem_profile_showing_state, 0);
	init_waitqueue_head(&kctx->mem_profile_wait);

	return kctx;

no_jit:
	kbase_gpu_vm_lock(kctx);
	kbase_sticky_resource_term(kctx);
	kbase_gpu_vm_unlock(kctx);
no_sticky:
	kbase_region_tracker_term(kctx);
no_region_tracker:
	kbase_mem_pool_free(&kctx->mem_pool, p, false);
no_sink_page:
	/* VM lock needed for the call to kbase_mmu_free_pgd */
	kbase_gpu_vm_lock(kctx);
	kbase_mmu_free_pgd(kctx);
	kbase_gpu_vm_unlock(kctx);
pgd_no_mem:
	kbase_mmu_term(kctx);
term_dma_fence:
	kbase_dma_fence_term(kctx);
free_event:
	kbase_event_cleanup(kctx);
free_jd:
	/* Safe to call this one even when didn't initialize (assuming kctx was sufficiently zeroed) */
	kbasep_js_kctx_term(kctx);
	kbase_jd_exit(kctx);
deinit_evictable:
	kbase_mem_evictable_deinit(kctx);
free_both_pools:
	kbase_mem_pool_term(&kctx->lp_mem_pool);
free_mem_pool:
	kbase_mem_pool_term(&kctx->mem_pool);
free_kctx:
	vfree(kctx);
out:
	return NULL;
}
KBASE_EXPORT_SYMBOL(kbase_create_context);

static void kbase_reg_pending_dtor(struct kbase_va_region *reg)
{
	dev_dbg(reg->kctx->kbdev->dev, "Freeing pending unmapped region\n");
	kbase_mem_phy_alloc_put(reg->cpu_alloc);
	kbase_mem_phy_alloc_put(reg->gpu_alloc);
	kfree(reg);
}

/**
 * kbase_destroy_context - Destroy a kernel base context.
 * @kctx: Context to destroy
 *
 * Calls kbase_destroy_os_context() to free OS specific structures.
 * Will release all outstanding regions.
 */
void kbase_destroy_context(struct kbase_context *kctx)
{
	struct kbase_device *kbdev;
	int pages;
	unsigned long pending_regions_to_clean;
	unsigned long flags;
	struct page *p;

	/* MALI_SEC_INTEGRATION */
	int profile_count;

	/* MALI_SEC_INTEGRATION */
	if (!kctx) {
		printk("An uninitialized or destroyed context is tried to be destroyed. kctx is null\n");
		return ;
	} else if (kctx->ctx_status != CTX_INITIALIZED) {
		printk("An uninitialized or destroyed context is tried to be destroyed\n");
		printk("kctx: 0x%p, kctx->tgid: %d, kctx->ctx_status: 0x%x\n", kctx, kctx->tgid, kctx->ctx_status);
		return ;
	}

	KBASE_DEBUG_ASSERT(NULL != kctx);

	kbdev = kctx->kbdev;
	KBASE_DEBUG_ASSERT(NULL != kbdev);

	/* MALI_SEC_INTEGRATION */
	for (profile_count = 0; profile_count < 3; profile_count++) {
		if (wait_event_timeout(kctx->mem_profile_wait, atomic_read(&kctx->mem_profile_showing_state) == 0, (unsigned int) msecs_to_jiffies(1000)))
			break;
		else
			printk("[G3D] waiting for memory profile\n");
	}

	/* MALI_SEC_INTEGRATION */
	while (wait_event_timeout(kbdev->pm.suspending_wait, kbdev->pm.suspending == false, (unsigned int) msecs_to_jiffies(1000)) == 0)
		printk("[G3D] Waiting for resuming the device\n");

	KBASE_TRACE_ADD(kbdev, CORE_CTX_DESTROY, kctx, NULL, 0u, 0u);

	/* Ensure the core is powered up for the destroy process */
	/* A suspend won't happen here, because we're in a syscall from a userspace
	 * thread. */
	kbase_pm_context_active(kbdev);

	kbase_jd_zap_context(kctx);

#ifdef CONFIG_DEBUG_FS
	/* Removing the rest of the debugfs entries here as we want to keep the
	 * atom debugfs interface alive until all atoms have completed. This
	 * is useful for debugging hung contexts. */
	debugfs_remove_recursive(kctx->kctx_dentry);
#endif

	kbase_event_cleanup(kctx);

	/*
	 * JIT must be terminated before the code below as it must be called
	 * without the region lock being held.
	 * The code above ensures no new JIT allocations can be made by
	 * by the time we get to this point of context tear down.
	 */
	kbase_jit_term(kctx);

	kbase_gpu_vm_lock(kctx);

	kbase_sticky_resource_term(kctx);

	/* MMU is disabled as part of scheduling out the context */
	kbase_mmu_free_pgd(kctx);

	/* drop the aliasing sink page now that it can't be mapped anymore */
	p = phys_to_page(as_phys_addr_t(kctx->aliasing_sink_page));
	kbase_mem_pool_free(&kctx->mem_pool, p, false);

	/* free pending region setups */
	pending_regions_to_clean = (~kctx->cookies) & KBASE_COOKIE_MASK;
	while (pending_regions_to_clean) {
		unsigned int cookie = __ffs(pending_regions_to_clean);

		BUG_ON(!kctx->pending_regions[cookie]);

		kbase_reg_pending_dtor(kctx->pending_regions[cookie]);

		kctx->pending_regions[cookie] = NULL;
		pending_regions_to_clean &= ~(1UL << cookie);
	}

	kbase_region_tracker_term(kctx);
	kbase_gpu_vm_unlock(kctx);

	/* Safe to call this one even when didn't initialize (assuming kctx was sufficiently zeroed) */
	kbasep_js_kctx_term(kctx);

	kbase_jd_exit(kctx);

	kbase_dma_fence_term(kctx);

	mutex_lock(&kbdev->mmu_hw_mutex);
	spin_lock_irqsave(&kctx->kbdev->hwaccess_lock, flags);
	kbase_ctx_sched_remove_ctx(kctx);
	spin_unlock_irqrestore(&kctx->kbdev->hwaccess_lock, flags);
	mutex_unlock(&kbdev->mmu_hw_mutex);

	kbase_mmu_term(kctx);

	pages = atomic_read(&kctx->used_pages);
	if (pages != 0)
		dev_warn(kbdev->dev, "%s: %d pages in use!\n", __func__, pages);

	kbase_mem_evictable_deinit(kctx);
	kbase_mem_pool_term(&kctx->mem_pool);
	kbase_mem_pool_term(&kctx->lp_mem_pool);
	WARN_ON(atomic_read(&kctx->nonmapped_pages) != 0);

	/* MALI_SEC_INTEGRATION */
	if (kbdev->vendor_callbacks->destroy_context)
		kbdev->vendor_callbacks->destroy_context(kctx);

	if (kctx->ctx_need_qos) {
		kctx->ctx_need_qos = false;
	}

	vfree(kctx);
	/* MALI_SEC_INTEGRATION */
	kctx = NULL;

	kbase_pm_context_idle(kbdev);
}
KBASE_EXPORT_SYMBOL(kbase_destroy_context);

/**
 * kbase_context_set_create_flags - Set creation flags on a context
 * @kctx: Kbase context
 * @flags: Flags to set
 *
 * Return: 0 on success
 */
int kbase_context_set_create_flags(struct kbase_context *kctx, u32 flags)
{
	int err = 0;
	struct kbasep_js_kctx_info *js_kctx_info;
	unsigned long irq_flags;

	KBASE_DEBUG_ASSERT(NULL != kctx);

	js_kctx_info = &kctx->jctx.sched_info;

	/* Validate flags */
	if (flags != (flags & BASE_CONTEXT_CREATE_KERNEL_FLAGS)) {
		err = -EINVAL;
		goto out;
	}

	mutex_lock(&js_kctx_info->ctx.jsctx_mutex);
	spin_lock_irqsave(&kctx->kbdev->hwaccess_lock, irq_flags);

	/* Translate the flags */
	if ((flags & BASE_CONTEXT_SYSTEM_MONITOR_SUBMIT_DISABLED) == 0)
		kbase_ctx_flag_clear(kctx, KCTX_SUBMIT_DISABLED);

	/* Latch the initial attributes into the Job Scheduler */
	kbasep_js_ctx_attr_set_initial_attrs(kctx->kbdev, kctx);

	spin_unlock_irqrestore(&kctx->kbdev->hwaccess_lock, irq_flags);
	mutex_unlock(&js_kctx_info->ctx.jsctx_mutex);
 out:
	return err;
}
KBASE_EXPORT_SYMBOL(kbase_context_set_create_flags);
