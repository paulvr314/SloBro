// SPDX-License-Identifier: GPL-2.0
/*
 * mm/mpc_endpoint.c
 *
 * handles the collection and reporting of the mpc curve for a cgroup.
 * puts page depths into bins (probably linearly spaced) and exports results to cgroup fs.
 */

#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/timekeeping.h>
#include <linux/printk.h>
#include <linux/uaccess.h>

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/mmzone.h>
#include <linux/swap.h>

#include <linux/mpc.h>
#include <linux/seq_file.h>


/* ---------------------------------------------------------------------
 * definitions and Data structures
 * --------------------------------------------------------------------- */

 #define PAGE_WALK_INTERVAL_MS 30000 

struct mpc_endpoint {
	atomic_t depth_bins[DEPTH_NR_BINS];
	u32 binwidth;
	u32 max_depth_bin;
	bool enabled;
    struct task_struct *thread;
};

/* ---------------------------------------------------------------------
 * MGLRU enabled checks
 * --------------------------------------------------------------------- */

#ifdef CONFIG_LRU_GEN

static bool mpc_mglru_active(void)
{
    return lru_gen_enabled();
}

#else

static inline bool mpc_mglru_active(void)
{
    return false;
}

#endif

/* ---------------------------------------------------------------------
 * buffer management
 * --------------------------------------------------------------------- */

 /* Maps a generation array index (0..MAX_NR_GENS-1) to its active sequence number */
static unsigned long mpc_gen_to_seq(struct lru_gen_folio *lrugen, int gen)
{
    unsigned long max_seq = READ_ONCE(lrugen->max_seq);
    unsigned long min_seq = READ_ONCE(lrugen->min_seq[LRU_GEN_ANON]);
    unsigned long seq;

    /* Search the active generation sequence window [min_seq, max_seq] */
    for (seq = min_seq; seq <= max_seq; seq++) {
        if (lru_gen_from_seq(seq) == gen)
            return seq;
    }

    /* Fallback if gen is out of active range */
    return min_seq;
}

//get total number of pages -- this may be too slow to run on every access
//in the future I may want to precalcuate this to save time
static unsigned long mpc_sum_anon_gens(struct lru_gen_folio *lrugen,
                                        unsigned long from_seq,
                                        unsigned long to_seq)
{
    unsigned long pages = 0;
    unsigned long seq;
    int zone;

    for (seq = from_seq; seq <= to_seq; seq++) {
        int g = lru_gen_from_seq(seq);
        for (zone = 0; zone < MAX_NR_ZONES; zone++)
            pages += lrugen->nr_pages[g][0][zone]; 
    }
    return pages;
}


//checks that page is anon and that mpc is enabled for the memcg
static inline bool mpc_should_track(struct folio *folio, struct mem_cgroup *memcg)
{
    if (folio_is_file_lru(folio))
        return false;
    if (!memcg || !memcg->mpc || !memcg->mpc->enabled)
        return false;
    return true;
}

static void record_depth(struct mpc_endpoint *mpc, u32 depth)
{
	u32 bin;

	if (!mpc->enabled)
		return;

	bin = depth / mpc->binwidth;
	if (bin > mpc->max_depth_bin)
		bin = mpc->max_depth_bin;

	atomic_inc(&mpc->depth_bins[bin]);
}

void mpc_hook_first_access(struct folio *folio)
{
    struct mem_cgroup *memcg = folio_memcg(folio);
    if (mpc_should_track(folio, memcg)) {
        record_depth(memcg->mpc, 0);
        atomic_inc(&memcg->mpc->depth_bins[memcg->mpc->max_depth_bin-4]);
    }
}

void mpc_hook_from_gen(struct folio *folio, struct lru_gen_folio *lrugen, struct mem_cgroup *memcg, int old_gen)
{
    if (!mpc_should_track(folio, memcg))
        return;

    /* Convert old_gen index to its true sequence number */
    unsigned long old_seq = mpc_gen_to_seq(lrugen, old_gen);
    unsigned long max_seq = READ_ONCE(lrugen->max_seq);

    unsigned long depth = 0;

    /* 1. Sum pages in strictly younger generations (old_seq + 1 up to max_seq) */
    if (old_seq < max_seq)
        depth += mpc_sum_anon_gens(lrugen, old_seq + 1, max_seq);

    /* 2. Add half of its own generation */
    depth += mpc_sum_anon_gens(lrugen, old_seq, old_seq) / 2;

    record_depth(memcg->mpc, depth);

    atomic_inc(&memcg->mpc->depth_bins[memcg->mpc->max_depth_bin-3]);
}

void mpc_hook_ws_refault(struct folio *folio, struct lru_gen_folio *lrugen)
{
    struct mem_cgroup *memcg = folio_memcg(folio);
    if (!mpc_should_track(folio, memcg))
        return;

    //total pages in mem
    unsigned long depth = mpc_sum_anon_gens(lrugen, lrugen->min_seq[0], lrugen->max_seq);
    record_depth(memcg->mpc, depth);

    atomic_inc(&memcg->mpc->depth_bins[memcg->mpc->max_depth_bin-2]);
}


void mpc_hook_slow_refault(struct folio *folio, struct lru_gen_folio *lrugen)
{
    struct mem_cgroup *memcg = folio_memcg(folio);
    if (!mpc_should_track(folio, memcg))
        return;

    //total pages in mem + half of total swap pages
    unsigned long depth = mpc_sum_anon_gens(lrugen, lrugen->min_seq[0], lrugen->max_seq);
    depth += (total_swap_pages - get_nr_swap_pages()) / 2;
    record_depth(memcg->mpc, depth);

    atomic_inc(&memcg->mpc->depth_bins[memcg->mpc->max_depth_bin-1]);
}

/* ---------------------------------------------------------------------
 * Export to cgroup fs
 * --------------------------------------------------------------------- */

int mpc_seq_show(struct seq_file *m, struct mpc_endpoint *mpc)
{
	if (!mpc)
		return 0;

	seq_printf(m, "%d %u %u\n", DEPTH_NR_BINS, mpc->binwidth, mpc->max_depth_bin);

	for (int i = 0; i < DEPTH_NR_BINS; i++) {
		seq_printf(m, "%d\n", atomic_read(&mpc->depth_bins[i]));
	}

	return 0;
}

/* ---------------------------------------------------------------------
 * Thread stuff (handles file export every 30s)
 * --------------------------------------------------------------------- */

//callback for pte
static void page_logger_pte_entry(pte_t *pte, unsigned long addr,
                                 unsigned long next, struct mm_walk *walk)
{
    struct folio *folio;
    struct mem_cgroup *memcg = walk->private;
    struct lruvec *lruvec;

    //check if the young bit is set, otherwise do nothing
    if (!pte_present(ptep_get(pte)))
        return;

    if (!ptep_test_and_clear_young(walk->vma, addr, pte))
        return;

    //if we cleared the young bit, we first have to check whether to count the folio towards the mpc
    //then once the mpc has been settled, we can activate the folio to move it to the youngest gen
    
    //get folio
    folio = vm_normal_folio(walk->vma, addr, ptep_get(pte));
    
    if (!folio || !folio_test_lru(folio))
        return;

    //check that the folio is anonymous and ignore flag is not set
    if (folio_test_anon(folio) && !folio_test_clear_counted(folio)) {
        lruvec = folio_lruvec(folio);
        old_gen = folio_lru_gen(folio);
        mpc_hook_from_gen(folio, &lruvec->lrugen, memcg, old_gen);
    }
    
    //check that folio is not already active, then activate it
    if (!folio_test_active(folio))
        folio_activate(folio);
}


//walk_ops struct for walk_page_range, to be used in process_mm()
static const struct mm_walk_ops page_logger_walk_ops = {
    .pgd_entry = NULL,
    .p4d_entry = NULL,
    .pud_entry = NULL,
    .pmd_entry = NULL,
    .pte_entry = page_logger_pte_entry,
    .pte_hole = NULL,
    .hugetlb_entry = NULL,
    .test_walk = NULL,
    .pre_vma = NULL,
    .post_vma = NULL,
    .walk_lock = PGWALK_WRLOCK
};


//walk page tables for each process in cgroup
static void scan_memcg(struct mem_cgroup *memcg)
{
    struct css_task_iter it;
    struct task_struct  *task;
    struct mm_struct    *mm;

    css_task_iter_start(&memcg->css, CSS_TASK_ITER_PROCS, &it);

    while ((task = css_task_iter_next(&it))) {
        mm = get_task_mm(task);
        if (!mm)
            continue;

        mmap_write_lock(mm);
        walk_page_range(mm, 0, TASK_SIZE, &page_logger_walk_ops, memcg);
        mmap_write_unlock(mm);
        mmput(mm);
    }

    css_task_iter_end(&it);
}

//I decided to have one thread per cgroup to avoid going through every cgroup.
static int mpc_thread_fn(void *data) 
{
    struct mem_cgroup *memcg = data;

    pr_info("new cgroup created -- page logging thread started\n");

    while (!kthread_should_stop()) {
        msleep_interruptible(PAGE_WALK_INTERVAL_MS);
        
    }

    pr_info("page_logger: thread stopping\n");
    return 0;
}


/* ---------------------------------------------------------------------
 * Init / teardown 
 * --------------------------------------------------------------------- */

 struct mpc_endpoint *mpc_endpoint_alloc(struct mem_cgroup *memcg)
 {
	if (!mpc_mglru_active())
		return NULL;

	struct mpc_endpoint *mpc = kzalloc(sizeof(struct mpc_endpoint), GFP_KERNEL);
	if (!mpc)
		return NULL;

	mpc->max_depth_bin = DEPTH_NR_BINS - 1;
    mpc->binwidth = MPC_MAX_DEPTH / DEPTH_NR_BINS;
	mpc->enabled = true;

    mpc->thread = kthread_run(mpc_thread_fn, memcg, "mpc_thread");

	return mpc;
 }

void mpc_endpoint_free(struct mpc_endpoint *mpc)
{
    if (!mpc)
        return;
    if (!IS_ERR_OR_NULL(mpc->thread))
        kthread_stop(mpc->thread);
    kfree(mpc);
}