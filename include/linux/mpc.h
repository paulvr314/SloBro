/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mm/mpc.h
 *
 * Declarations for MPC (memory performance curve) tracking.
 *
 * This header is intentionally thin: it only forward-declares the types
 * it needs and exposes function prototypes that take pointers. It must
 * NOT include <linux/memcontrol.h>, since this header is included from
 * <linux/mm_inline.h>, which is included from memcontrol.c itself --
 * pulling the full memcontrol.h in here would risk a circular include.
 * Any code that needs the full definition of struct mem_cgroup (or
 * needs to dereference memcg->mpc) belongs in mpc.c, not in this header.
 */

#ifndef _MPC_H
#define _MPC_H

#define DEPTH_NR_BINS         1000
#define MPC_MAX_DEPTH         750000

/* Forward declarations only -- these types are used exclusively as
 * pointers in this header, so we never need their full definitions
 * here. Full definitions are pulled in by mpc.c as needed.
 */
struct folio;
struct mem_cgroup;
struct lru_gen_folio;
struct mpc_endpoint;

/* ---------------------------------------------------------------------
 * folio counted flag helpers
 * --------------------------------------------------------------------- */

//check page counted and clear
bool mpc_page_already_counted(struct folio *folio);

//set page counted
void mpc_set_page_counted(struct folio *folio);

/* ---------------------------------------------------------------------
 * Access-recording hooks
 *
 * These are called from the various MGLRU access/refault paths
 * (mm_inline.h, workingset.c) to record a page-access depth sample.
 * Each hook internally checks mpc_should_track() (anon-only, mpc
 * enabled) before doing any real work, so callers do not need to
 * pre-filter -- just call unconditionally at the relevant hook site.
 * --------------------------------------------------------------------- */

/* Case 4: first-time access (folio just assigned its first generation) */
void mpc_hook_first_access(struct folio *folio);

/* Case 1: in-memory re-access; old_gen/new_gen bound the generation
 * range needed for the depth calculation (younger gens + half own gen).
 */
void mpc_hook_from_gen(struct folio *folio, unsigned long old_gen,
			unsigned long new_gen, struct lru_gen_folio *lrugen,
			struct mem_cgroup *memcg);

/* Case 2: working-set (fast) refault */
void mpc_hook_ws_refault(struct folio *folio, struct lru_gen_folio *lrugen);

/* Case 3: slow refault (from swap) */
void mpc_hook_slow_refault(struct folio *folio, struct lru_gen_folio *lrugen);

/* Display MPC metrics in cgroup seq_file */
int mpc_seq_show(struct seq_file *m, struct mpc_endpoint *mpc);

//use for testing
void mpc_log_skip(struct folio *folio);

/* ---------------------------------------------------------------------
 * Init / teardown
 *
 * Called from mem_cgroup_css_alloc()/mem_cgroup_css_free() (or
 * equivalent) in memcontrol.c. That file already includes the full
 * memcontrol.h itself, so passing struct mem_cgroup * here by pointer
 * is safe -- mpc.h only needs the forward declaration above.
 * --------------------------------------------------------------------- */

struct mpc_endpoint *mpc_endpoint_alloc(struct mem_cgroup *memcg);
void mpc_endpoint_free(struct mpc_endpoint *mpc);

#endif /* _MPC_H */