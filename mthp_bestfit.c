// SPDX-License-Identifier: GPL-2.0
/*
 * mthp_bestfit.c v3.0  --  VMA-size best-fit mTHP order selection
 *
 * Changes vs v2.x:
 *   - Uses android_vh_thp_vma_allowable_orders vendor hook instead of
 *     kretprobe on __thp_vma_allowable_orders. No kallsyms lookup,
 *     no LLVM-LTO symbol rename hassle, no maxactive tuning.
 *   - No kprobe dependencies in Kconfig.
 *   - Cleaner unload via tracepoint_synchronize_unregister().
 *
 * Requires Android Common Kernel with the hook present. Verify:
 *     grep android_vh_thp_vma_allowable_orders \
 *          include/trace/hooks/mm.h drivers/android/vendor_hooks.c
 *
 * Added to ACK in android15-6.6 (Aug 2024). Cherry-picked to later branches
 * including android15-6.12 and android16-6.12.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/huge_mm.h>
#include <linux/sysctl.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/percpu.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>
#include <linux/sizes.h>

#include <trace/hooks/mm.h>

#define MOD_NAME "mthp_bestfit"
#define MOD_VER  "3.0"

/* -- contPTE order per base page size ------------------------------------ */
#if PAGE_SIZE == SZ_4K
# define CONTPTE_ORDER 4       /* 64 KB = 16 x 4 KB */
#elif PAGE_SIZE == SZ_16K
# define CONTPTE_ORDER 0       /* contPTE larger than mTHP range we target */
#else
# define CONTPTE_ORDER 0
#endif

/* -- sysctls ------------------------------------------------------------- */
static int sysctl_mthp_bestfit_enabled     __read_mostly = 1;
static int sysctl_mthp_bestfit_min_pages   __read_mostly = 2;
static int sysctl_mthp_bestfit_contpte_bias __read_mostly = 1;

/* Bounds owned by this module. Mixing SYSCTL_ZERO/SYSCTL_ONE (kernel-owned
 * const pointers) with module-owned int* pointers in the same ctl_table
 * caused __register_sysctl_table() to silently return NULL on 6.12 GKI
 * hardened builds. Uniform module-owned int* pointers sidesteps that. */
static int bf_int_zero = 0;
static int bf_int_one  = 1;
static int bf_min_pages_max = 4;

/* -- per-CPU statistics -------------------------------------------------- */
struct bestfit_counters {
	u64 decisions;
	u64 order_hist[PMD_ORDER + 1];
	u64 contpte_hits;
	u64 capped_down;
	u64 stack_capped;
};

static DEFINE_PER_CPU(struct bestfit_counters, bestfit_pcpu);

static inline void bestfit_count(int order, bool contpte, bool capped,
				 bool stack)
{
	struct bestfit_counters *c;

	preempt_disable();
	c = this_cpu_ptr(&bestfit_pcpu);
	c->decisions++;
	if (order >= 0 && order <= PMD_ORDER)
		c->order_hist[order]++;
	if (contpte) c->contpte_hits++;
	if (capped)  c->capped_down++;
	if (stack)   c->stack_capped++;
	preempt_enable();
}

/* -- core best-fit decision --------------------------------------------- */
static int mthp_bestfit_order(struct vm_area_struct *vma,
			      bool *contpte, bool *capped, bool *stack)
{
	unsigned long vma_size, min_pages;
	int order;

	*contpte = *capped = *stack = false;

	if (!READ_ONCE(sysctl_mthp_bestfit_enabled))
		return -1;

	vma_size = vma->vm_end - vma->vm_start;
	min_pages = (unsigned long)READ_ONCE(sysctl_mthp_bestfit_min_pages);
	if (min_pages < 1)
		min_pages = 1;

	/* Stack: cap at order 2 (4x base-page bytes) -- stacks grow
	 * unpredictably, don't speculatively allocate huge. */
	if (vma->vm_flags & VM_GROWSDOWN) {
		*stack = true;
		if (vma_size >= (min_pages << (PAGE_SHIFT + 2)))
			return 2;
		return 0;
	}

	/* Too small even for smallest tracked order */
	if (vma_size < (min_pages << (PAGE_SHIFT + 2)))
		return 0;

	/* PMD fast-path: VMA big enough for a full PMD */
	if (vma_size >= (1UL << (PAGE_SHIFT + PMD_ORDER))) {
		order = PMD_ORDER;
		goto boost;
	}

	/* Walk down from PMD-1 to the largest order where min_pages fit. */
	for (order = PMD_ORDER - 1; order >= 2; order--) {
		unsigned long page_size = 1UL << (PAGE_SHIFT + order);
		if (vma_size >= min_pages * page_size) {
			*capped = true;
			goto boost;
		}
	}
	return 0;

boost:
	/* contPTE bias: bump small orders up to CONTPTE_ORDER when the VMA
	 * is large enough to fill at least one contPTE folio. 16x TLB
	 * compression vs 4 KB on A76. Only applies on 4 KB base pages. */
	if (READ_ONCE(sysctl_mthp_bestfit_contpte_bias) &&
	    CONTPTE_ORDER > 0 &&
	    order > 0 && order < CONTPTE_ORDER &&
	    vma_size >= (1UL << (PAGE_SHIFT + CONTPTE_ORDER))) {
		order = CONTPTE_ORDER;
		*contpte = true;
	}
	if (order == CONTPTE_ORDER && CONTPTE_ORDER > 0)
		*contpte = true;
	return order;
}

static unsigned long __mthp_bestfit_mask(struct vm_area_struct *vma,
					 unsigned long allowed_orders)
{
	int best;
	bool contpte, capped, stack;

	if (!READ_ONCE(sysctl_mthp_bestfit_enabled))
		return allowed_orders;

	best = mthp_bestfit_order(vma, &contpte, &capped, &stack);
	if (best < 0)
		return allowed_orders;

	bestfit_count(best, contpte, capped, stack);

	if (best >= PMD_ORDER)
		return allowed_orders;

	/* Mask off orders above 'best'. Keep 0..best inclusive. */
	return allowed_orders & ((1UL << (best + 1)) - 1);
}

/* -- vendor hook handler ------------------------------------------------- */
/*
 * android_vh_thp_vma_allowable_orders is called from thp_vma_allowable_orders
 * with a pointer to the orders bitmask. Our handler modifies the bitmask
 * in place. Runs under mmap_read_lock; no sleeping allowed.
 *
 * Signature per ACK:
 *   trace_android_vh_thp_vma_allowable_orders(struct vm_area_struct *vma,
 *                                             unsigned long *orders);
 */
static void bestfit_vh_allowable_orders(void *data,
					struct vm_area_struct *vma,
					unsigned long *orders)
{
	(void)data;
	if (unlikely(!vma || !orders))
		return;
	if (!*orders)
		return;
	*orders = __mthp_bestfit_mask(vma, *orders);
}

/* -- sysctl table -------------------------------------------------------- */
static struct ctl_table mthp_bestfit_sysctls[] = {
	{
		.procname	= "enabled",
		.data		= &sysctl_mthp_bestfit_enabled,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &bf_int_zero,
		.extra2		= &bf_int_one,
	},
	{
		.procname	= "min_pages",
		.data		= &sysctl_mthp_bestfit_min_pages,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &bf_int_one,
		.extra2		= &bf_min_pages_max,
	},
	{
		.procname	= "contpte_bias",
		.data		= &sysctl_mthp_bestfit_contpte_bias,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &bf_int_zero,
		.extra2		= &bf_int_one,
	},
};

static struct ctl_table_header *mthp_bestfit_sysctl_hdr;

/* -- debugfs ------------------------------------------------------------- */
#ifdef CONFIG_DEBUG_FS
static struct dentry *mthp_bestfit_debugfs;

static const char * const order_names[] = {
	[0] = "  4KB", [1] = "  8KB", [2] = " 16KB", [3] = " 32KB",
	[4] = " 64KB", [5] = "128KB", [6] = "256KB", [7] = "512KB",
	[8] = "  1MB", [9] = "  2MB",
};

static void collect_totals(struct bestfit_counters *total)
{
	int cpu, i;

	memset(total, 0, sizeof(*total));
	for_each_possible_cpu(cpu) {
		struct bestfit_counters *c = per_cpu_ptr(&bestfit_pcpu, cpu);
		total->decisions    += READ_ONCE(c->decisions);
		total->contpte_hits += READ_ONCE(c->contpte_hits);
		total->capped_down  += READ_ONCE(c->capped_down);
		total->stack_capped += READ_ONCE(c->stack_capped);
		for (i = 0; i <= PMD_ORDER; i++)
			total->order_hist[i] += READ_ONCE(c->order_hist[i]);
	}
}

static int bestfit_stats_show(struct seq_file *m, void *v)
{
	struct bestfit_counters total;
	int i;

	collect_totals(&total);

	seq_printf(m, "mthp_bestfit v%s statistics\n", MOD_VER);
	seq_puts(m, "================================\n");
	seq_printf(m, "enabled:        %d\n", sysctl_mthp_bestfit_enabled);
	seq_printf(m, "min_pages:      %d\n", sysctl_mthp_bestfit_min_pages);
	seq_printf(m, "contpte_bias:   %d\n", sysctl_mthp_bestfit_contpte_bias);
	seq_printf(m, "contpte_order:  %d (%luKB)\n", CONTPTE_ORDER,
		   (1UL << (PAGE_SHIFT + CONTPTE_ORDER)) / 1024);

	seq_printf(m, "\nTotal decisions: %llu\n", total.decisions);
	seq_printf(m, "contPTE selected: %llu (%llu%%)\n", total.contpte_hits,
		   total.decisions > 0 ?
		   total.contpte_hits * 100 / total.decisions : 0);
	seq_printf(m, "Capped below max: %llu (%llu%%)\n", total.capped_down,
		   total.decisions > 0 ?
		   total.capped_down * 100 / total.decisions : 0);
	seq_printf(m, "Stack capped:     %llu\n", total.stack_capped);

	seq_puts(m, "\nOrder histogram:\n");
	for (i = 0; i <= PMD_ORDER; i++) {
		u64 cnt = total.order_hist[i];
		u64 pct;
		if (cnt == 0)
			continue;
		pct = total.decisions > 0 ?
		      cnt * 100 / total.decisions : 0;
		seq_printf(m, "  order %d (%s): %10llu  (%3llu%%)%s\n",
			   i,
			   i < ARRAY_SIZE(order_names) ? order_names[i] : "?",
			   cnt, pct,
			   i == CONTPTE_ORDER && CONTPTE_ORDER > 0 ?
			     "  [contPTE]" :
			   i == PMD_ORDER ? "  [PMD]" : "");
	}

	seq_puts(m, "\nBest-fit table (assuming current min_pages):\n");
	for (i = 2; i < PMD_ORDER; i++) {
		unsigned long min_vma =
			(unsigned long)sysctl_mthp_bestfit_min_pages
			<< (PAGE_SHIFT + i);
		seq_printf(m, "  VMA >= %6luKB -> order %d (%s)%s\n",
			   min_vma / 1024, i,
			   i < ARRAY_SIZE(order_names) ? order_names[i] : "?",
			   i == CONTPTE_ORDER && CONTPTE_ORDER > 0 ?
			     "  [contPTE]" : "");
	}
	seq_printf(m, "  VMA >= %6luKB -> order %d  [PMD]\n",
		   (1UL << (PAGE_SHIFT + PMD_ORDER)) / 1024, PMD_ORDER);

	return 0;
}

static int bestfit_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, bestfit_stats_show, NULL);
}

/* Write-to-reset: zero all per-CPU counters. Used by benchmark harnesses. */
static ssize_t bestfit_stats_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	int cpu;
	for_each_possible_cpu(cpu)
		memset(per_cpu_ptr(&bestfit_pcpu, cpu), 0,
		       sizeof(struct bestfit_counters));
	return count;
}

static const struct file_operations bestfit_stats_fops = {
	.open    = bestfit_stats_open,
	.read    = seq_read,
	.write   = bestfit_stats_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Machine-parseable live view: key value. */
static int bestfit_live_show(struct seq_file *m, void *v)
{
	struct bestfit_counters total;
	int i;

	collect_totals(&total);
	seq_printf(m, "# mthp_bestfit v%s live view\n", MOD_VER);
	seq_printf(m, "enabled %d\n", sysctl_mthp_bestfit_enabled);
	seq_printf(m, "min_pages %d\n", sysctl_mthp_bestfit_min_pages);
	seq_printf(m, "contpte_bias %d\n", sysctl_mthp_bestfit_contpte_bias);
	seq_printf(m, "contpte_order %d\n", CONTPTE_ORDER);
	seq_printf(m, "decisions %llu\n", total.decisions);
	seq_printf(m, "contpte_hits %llu\n", total.contpte_hits);
	seq_printf(m, "capped_down %llu\n", total.capped_down);
	seq_printf(m, "stack_capped %llu\n", total.stack_capped);
	for (i = 0; i <= PMD_ORDER; i++)
		if (total.order_hist[i])
			seq_printf(m, "order_%d %llu\n", i,
				   total.order_hist[i]);
	return 0;
}

static int bestfit_live_open(struct inode *inode, struct file *file)
{ return single_open(file, bestfit_live_show, NULL); }

static const struct file_operations bestfit_live_fops = {
	.open    = bestfit_live_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* A/B toggle: parallel to the sysctl knob, exposed via debugfs so scripts
 * don't have to worry about whether /proc/sys/vm/mthp_bestfit got created
 * on hardened builds. */
static int bestfit_enabled_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_mthp_bestfit_enabled);
	return 0;
}

static int bestfit_enabled_open(struct inode *inode, struct file *file)
{ return single_open(file, bestfit_enabled_show, NULL); }

static ssize_t bestfit_enabled_write(struct file *file, const char __user *buf,
				     size_t count, loff_t *ppos)
{
	char kbuf[8];
	size_t n = min(count, sizeof(kbuf) - 1);

	if (copy_from_user(kbuf, buf, n))
		return -EFAULT;
	kbuf[n] = '\0';
	WRITE_ONCE(sysctl_mthp_bestfit_enabled,
		   (kbuf[0] >= '1' && kbuf[0] <= '9') ? 1 : 0);
	return count;
}

static const struct file_operations bestfit_enabled_fops = {
	.open    = bestfit_enabled_open,
	.read    = seq_read,
	.write   = bestfit_enabled_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int mthp_bestfit_debugfs_init(void)
{
	mthp_bestfit_debugfs = debugfs_create_dir("mthp_bestfit", NULL);
	if (IS_ERR(mthp_bestfit_debugfs))
		return PTR_ERR(mthp_bestfit_debugfs);

	debugfs_create_file("stats",   0644, mthp_bestfit_debugfs,
			    NULL, &bestfit_stats_fops);
	debugfs_create_file("live",    0444, mthp_bestfit_debugfs,
			    NULL, &bestfit_live_fops);
	debugfs_create_file("enabled", 0644, mthp_bestfit_debugfs,
			    NULL, &bestfit_enabled_fops);

	/* Legacy flat paths so existing scripts keep working. */
	debugfs_create_file("mthp_bestfit_stats",   0644, NULL, NULL,
			    &bestfit_stats_fops);
	debugfs_create_file("mthp_bestfit_live",    0444, NULL, NULL,
			    &bestfit_live_fops);
	debugfs_create_file("mthp_bestfit_enabled", 0644, NULL, NULL,
			    &bestfit_enabled_fops);

	return 0;
}

static void mthp_bestfit_debugfs_exit(void)
{
	debugfs_remove_recursive(mthp_bestfit_debugfs);
}
#else
static inline int mthp_bestfit_debugfs_init(void)  { return 0; }
static inline void mthp_bestfit_debugfs_exit(void) {}
#endif /* CONFIG_DEBUG_FS */

/* -- module init / exit -------------------------------------------------- */
static int __init mthp_bestfit_init(void)
{
	int ret;

	mthp_bestfit_sysctl_hdr = register_sysctl("vm/mthp_bestfit",
						  mthp_bestfit_sysctls);
	if (!mthp_bestfit_sysctl_hdr)
		pr_warn(MOD_NAME ": register_sysctl failed "
			"(hardened build?), continuing with debugfs only\n");

	ret = register_trace_android_vh_thp_vma_allowable_orders(
			bestfit_vh_allowable_orders, NULL);
	if (ret) {
		pr_err(MOD_NAME
		       ": register vendor hook failed: %d\n", ret);
		if (mthp_bestfit_sysctl_hdr)
			unregister_sysctl_table(mthp_bestfit_sysctl_hdr);
		return ret;
	}

	ret = mthp_bestfit_debugfs_init();
	if (ret) {
		pr_warn(MOD_NAME ": debugfs init failed: %d\n", ret);
		/* non-fatal */
	}

	pr_info(MOD_NAME " v%s loaded (PMD_ORDER=%d CONTPTE_ORDER=%d)\n",
		MOD_VER, PMD_ORDER, CONTPTE_ORDER);
	return 0;
}

static void __exit mthp_bestfit_exit(void)
{
	unregister_trace_android_vh_thp_vma_allowable_orders(
		bestfit_vh_allowable_orders, NULL);

	/* CRITICAL: tracepoint callbacks may still be running on other CPUs
	 * when unregister returns. Wait for the RCU grace period before
	 * freeing module text. */
	tracepoint_synchronize_unregister();

	mthp_bestfit_debugfs_exit();
	if (mthp_bestfit_sysctl_hdr)
		unregister_sysctl_table(mthp_bestfit_sysctl_hdr);

	pr_info(MOD_NAME " v" MOD_VER " unloaded\n");
}

module_init(mthp_bestfit_init);
module_exit(mthp_bestfit_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("mTHP Autopilot");
MODULE_DESCRIPTION("VMA-size best-fit mTHP order selection via Android vendor hook");
MODULE_VERSION(MOD_VER);
