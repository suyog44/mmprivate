/*
 * mthp_bestfit.c (refined)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/huge_mm.h>
#include <linux/mmzone.h>
#include <linux/percpu.h>
#include <linux/topology.h>
#include <trace/hooks/mm.h>

#define BF_NAME "mthp_bestfit"
#define BF_VER  "4.2"

/* ---- Sysctls ---- */
static int sysctl_enabled __read_mostly = 1;
static int sysctl_min_pages __read_mostly = 2;
static int sysctl_exec_boost __read_mostly = 1;
static int sysctl_dry_run __read_mostly = 0;
static int sysctl_numa_aware __read_mostly = 1;
static int sysctl_lifetime_aware __read_mostly = 1;

/* ---- Counters ---- */
struct bf_counters {
	u64 decisions;
	u64 pressure_downgraded;
	u64 numa_downgraded;
	u64 lifetime_conserved;
	u64 too_small;
	u64 mask_empty;
};

static DEFINE_PER_CPU(struct bf_counters, bf_pcpu);

/* ---- Counter helper ---- */
static inline void bf_count(bool too_small,
			   bool pressure_dg,
			   bool numa_dg,
			   bool lifetime_c,
			   bool mask_empty)
{
	struct bf_counters *c = this_cpu_ptr(&bf_pcpu);

	c->decisions++;

	if (too_small)
		c->too_small++;
	if (pressure_dg)
		c->pressure_downgraded++;
	if (numa_dg)
		c->numa_downgraded++;
	if (lifetime_c)
		c->lifetime_conserved++;
	if (mask_empty)
		c->mask_empty++;
}

/* ---- Watermark-aware availability ---- */
static bool bf_order_available(struct zone *zone, int order)
{
	return zone_watermark_ok(zone, order,
		min_wmark_pages(zone),
		0, ALLOC_WMARK_LOW);
}

/* ---- Unified limit (NUMA + pressure) ---- */
static int bf_limit_order(int start,
			 bool *pressure_dg,
			 bool *numa_dg)
{
	int order, nid = numa_node_id(), z;
	struct pglist_data *pgdat = NODE_DATA(nid);
	struct zone *zone;

	for (order = start; order >= 2; order--) {

		/* 1. Try local node first */
		if (READ_ONCE(sysctl_numa_aware)) {
			for (z = 0; z < MAX_NR_ZONES; z++) {
				zone = &pgdat->node_zones[z];

				if (!populated_zone(zone))
					continue;

				if (bf_order_available(zone, order))
					return order;
			}
		}

		/* 2. Fallback global */
		for_each_populated_zone(zone) {
			if (bf_order_available(zone, order)) {
				if (READ_ONCE(sysctl_numa_aware))
					*numa_dg = true;
				return order;
			}
		}

		*pressure_dg = true;
	}

	*pressure_dg = true;
	*numa_dg = true;
	return 2;
}

/* ---- Core selection ---- */
static int bf_select_order(const struct vm_area_struct *vma,
			   bool *pressure_dg,
			   bool *numa_dg,
			   bool *lifetime_c)
{
	unsigned long min_pages, page_size, first_aligned;
	bool is_exec;
	int order;

	*pressure_dg = false;
	*numa_dg = false;
	*lifetime_c = false;

	if (!READ_ONCE(sysctl_enabled))
		return -1;

	min_pages = READ_ONCE(sysctl_min_pages);

	/* Fast reject small VMAs */
	page_size = 1UL << (PAGE_SHIFT + 2);
	first_aligned = ALIGN(vma->vm_start, page_size);
	if (first_aligned + page_size > vma->vm_end)
		return 0;

	is_exec = (vma->vm_flags & VM_EXEC) && vma->vm_file;

	/* Geometric best-fit */
	for (order = PMD_ORDER; order >= 2; order--) {
		page_size = 1UL << (PAGE_SHIFT + order);
		first_aligned = ALIGN(vma->vm_start, page_size);

		if (first_aligned < vma->vm_end &&
		    (vma->vm_end - first_aligned) >= min_pages * page_size)
			break;
	}

	if (order < 2)
		return 0;

	/* Apply pressure + NUMA limit */
	order = bf_limit_order(order, pressure_dg, numa_dg);

	/* Exec boost (safe) */
	if (is_exec && READ_ONCE(sysctl_exec_boost) &&
	    order < PMD_ORDER)
		order++;

	/* Lifetime heuristic */
	if (READ_ONCE(sysctl_lifetime_aware) &&
	    vma_is_anonymous(vma) &&
	    !vma->anon_vma &&
	    !(vma->vm_flags & VM_LOCKED) &&
	    order > 2) {
		order--;
		*lifetime_c = true;
	}

	return order;
}

/* ---- Hook handler ---- */
static void bestfit_vh_handler(void *data,
			      struct vm_area_struct *vma,
			      unsigned long *orders)
{
	bool pressure_dg, numa_dg, lifetime_c;
	unsigned long new_orders;
	int best;

	if (unlikely(!vma || !orders))
		return;

	if (*orders == 0)
		return;

	best = bf_select_order(vma,
			       &pressure_dg,
			       &numa_dg,
			       &lifetime_c);

	if (best < 0)
		return;

	if (best == 0) {
		bf_count(true, false, false, false, false);
		return;
	}

	if (best >= PMD_ORDER) {
		bf_count(false, pressure_dg, numa_dg, lifetime_c, false);
		return;
	}

	new_orders = *orders & ((1UL << (best + 1)) - 1);

	if (!new_orders) {
		bf_count(false, pressure_dg, numa_dg, lifetime_c, true);
		return;
	}

	bf_count(false, pressure_dg, numa_dg, lifetime_c, false);

	if (!READ_ONCE(sysctl_dry_run))
		*orders = new_orders;
}

/* ---- Init / Exit ---- */
static int __init mthp_bestfit_init(void)
{
	int ret;

	ret = register_trace_android_vh_thp_vma_allowable_orders(
		bestfit_vh_handler, NULL);

	if (ret) {
		pr_err(BF_NAME ": hook failed: %d\n", ret);
		return ret;
	}

	pr_info(BF_NAME " v" BF_VER " loaded\n");
	return 0;
}

static void __exit mthp_bestfit_exit(void)
{
	unregister_trace_android_vh_thp_vma_allowable_orders(
		bestfit_vh_handler, NULL);

	tracepoint_synchronize_unregister();

	pr_info(BF_NAME " v" BF_VER " unloaded\n");
}

module_init(mthp_bestfit_init);
module_exit(mthp_bestfit_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Suyog");
MODULE_DESCRIPTION("Refined VMA best-fit mTHP order selection");
MODULE_VERSION(BF_VER);