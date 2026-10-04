// SPDX-License-Identifier: GPL-2.0
/*
 * mm/page_logger.c
 *
 * added by paul
 * contains page table walking functionality given a cgroup.
 */

#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/timekeeping.h>
#include <linux/printk.h>
#include <linux/uaccess.h>
#include <linux/pagewalk.h>     /* mm_walk_ops, walk_page_range      */

#include <linux/memcontrol.h>   /* mem_cgroup, mem_cgroup_from_css        */
#include <linux/cgroup.h>       /* css_for_each_descendant_pre,
                                   css_task_iter_start/next/end            */
#include <linux/sched/mm.h>     /* get_task_mm(), mmput()                  */
#include <linux/mm.h>           /* mm_struct, mmap_read_lock/unlock        */



//callback for pte
static void page_logger_pte_entry(pte_t *pte, unsigned long addr,
                                 unsigned long next, struct mm_walk *walk)
{
    struct folio *folio;
    struct mem_cgroup *memcg = walk->private;

    //check if the young bit is set, otherwise do nothing
    if (!pte_present(ptep_get(pte)))
        return;

    if (!ptep_test_and_clear_young(walk->vma, addr, pte))
        return;

    //if we cleared the young bit, we first have to check whether to count the folio towards the mpc
    //then once the mpc has been settled, we can activate the folio to move it to the youngest gen
    folio = vm_normal_folio(walk->vma, addr, ptep_get(pte));
    if (folio && folio_test_anon(folio) && folio_test_lru(folio))
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