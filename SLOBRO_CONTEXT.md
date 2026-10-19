# SloBro — Project Context (branch `mglru_mpc`)

_Last updated 2026-10-19. Base: Linux 6.6 (`master` / merge-base `ffc253263a13`). Branch HEAD: `b5054f97aaec`._

## 1. Goal

SloBro builds a **Memory Performance Curve (MPC)** for each memory cgroup. The curve plots the number of page faults (y) against memory size (x).

For each observed anonymous-page access, the kernel records a **depth**: the smallest amount of memory (in pages) the cgroup would need for that access *not* to fault. This is roughly an LRU stack distance, approximated with MGLRU generations. Depths go into a histogram. The fault count at memory size M is the number of samples with depth > M (a reverse cumulative sum).

Scope:
- **MGLRU only.** If `lru_gen_enabled()` is false when a cgroup is created, no endpoint is allocated.
- **Anonymous pages only.** `folio_is_file_lru()` folios are ignored.
- **Per non-root memcg.**

## 2. Architecture

### 2.1 Files touched

| File | Change |
|---|---|
| `include/linux/mpc.h` (new) | Thin header with forward declarations, hook prototypes and bin constants. It deliberately avoids including `memcontrol.h`, because it is included from `mm_inline.h`. |
| `mm/mpc.c` (new) | `struct mpc_endpoint`, the depth math, the hooks, seq_file output and the per-cgroup page-walk kthread. |
| `mm/Makefile` | `mpc.o` is built under `CONFIG_MEMCG`. |
| `include/linux/memcontrol.h` | Adds `struct mpc_endpoint *mpc` to `struct mem_cgroup`. |
| `mm/memcontrol.c` | Calls `mpc_endpoint_alloc()` in `mem_cgroup_css_alloc()` (not for root) and `mpc_endpoint_free()` in `css_free`. Adds the cgroup file `memory.mpc` (`CFTYPE_NOT_ON_ROOT`). |
| `include/linux/page-flags.h`, `include/trace/events/mmflags.h` | New page flag `PG_counted`, with folio helpers `folio_{test,set,clear}_counted` and `folio_test_{set,clear}_counted`. |
| `mm/memory.c` | First-access hooks in `do_anonymous_page()` and `wp_page_copy()` (COW). |
| `mm/vmscan.c` | In-memory re-access hooks in `walk_pte_range()` (MGLRU aging walk) and `lru_gen_look_around()` (rmap look-around during eviction). |
| `mm/workingset.c` | Refault hooks in `lru_gen_refault()`. Also adds an unconditional `folio_set_active(folio)`. |
| `include/linux/mm_inline.h` | Only a comment: the old first-access hook in `lru_gen_add_folio()` was removed because it overcounted (it fired on LRU maintenance re-adds and double-counted refaults). |

### 2.2 `struct mpc_endpoint` (`mm/mpc.c`)

```c
struct mpc_endpoint {
    atomic_t depth_bins[DEPTH_NR_BINS];   // 1000 bins
    u32 binwidth;                         // MPC_MAX_DEPTH / DEPTH_NR_BINS = 750 pages
    u32 max_depth_bin;                    // DEPTH_NR_BINS - 1 (overflow bin)
    bool enabled;                         // always true once allocated
    struct task_struct *thread;           // per-cgroup page-walk kthread
};
```

- `MPC_MAX_DEPTH = 750000` pages, about 2.86 GiB with 4 KiB pages, so each bin is about 2.9 MiB. An older commit message said 5,000,000; the constant has since changed.
- Linear binning. Any depth ≥ `MPC_MAX_DEPTH` is clamped into the last bin.

### 2.3 The four access cases and their depth

| Case | Hook | Call site | Depth recorded | Sets `PG_counted`? |
|---|---|---|---|---|
| 4: first access | `mpc_hook_first_access` | `do_anonymous_page`, `wp_page_copy` | **0** | yes |
| 1: in-memory re-access (young bit seen) | `mpc_hook_from_gen` | `walk_pte_range`, `lru_gen_look_around`, mpc kthread | pages in strictly younger anon gens + ½ of the page's own gen | no; it *consumes* (test-and-clears) the bit and skips the sample if it was set |
| 2: "working-set" refault (`recent == true`) | `mpc_hook_ws_refault` | `lru_gen_refault` | all resident anon pages in the lruvec (`min_seq[ANON]..max_seq`) | yes |
| 3: "slow" refault (`recent == false`) | `mpc_hook_slow_refault` | `lru_gen_refault` | resident anon pages + ½ × (system-wide swap in use) | yes |

All hooks call `mpc_should_track()` first, which checks that the folio is anon and the memcg has an enabled endpoint, so call sites don't need to filter.

### 2.4 The `PG_counted` de-duplication scheme

A fault (first touch, COW or refault) also leaves the PTE **young**. The next MGLRU aging walk or look-around would then see that young bit and record a second, bogus in-memory access. To prevent this:

1. The fault-path hooks record their sample and **set** `PG_counted`.
2. `mpc_hook_from_gen` calls `mpc_page_already_counted()` (`folio_test_clear_counted`). If the bit was set, it clears the bit and **skips** the sample. That young bit was the fault itself.

The flag is cleared automatically when a page is freed (`PAGE_FLAGS_CHECK_AT_PREP`), so it never carries over across swap-out and swap-in.

### 2.5 Sampling sources for in-memory accesses

In-memory accesses are seen only when someone tests and clears the PTE accessed bit. Several accesses between two scans collapse into one sample. There are three scanners:

1. **MGLRU aging** (`walk_pte_range`, during `try_to_inc_max_seq`). This runs only under memory pressure or aging.
2. **MGLRU look-around** (`lru_gen_look_around`, from `folio_referenced` during eviction).
3. **SloBro kthread** (`mpc_thread_fn`, `mm/mpc.c`). There is one kthread per cgroup. Every 30 s (`PAGE_WALK_INTERVAL_MS`) it walks every process in the cgroup with `walk_page_range` (`PGWALK_WRLOCK`). For each young PTE it clears the young bit, calls `mpc_hook_from_gen`, moves dirtiness to the folio, and calls `folio_activate()`, copying `lru_gen_look_around`. After each mm it runs `flush_tlb_mm()` (TODO in the code: discuss with Shaurya whether flushing changes Linux behaviour). This gives regular samples even when there is no memory pressure.

### 2.6 Output: `memory.mpc`

```
<DEPTH_NR_BINS> <binwidth> <max_depth_bin>
<count bin 0>
...
<count bin 999>
```

**Debug instrumentation in the histogram:** each hook also increments one of the last 4 bins as a per-path counter:
`max-4` = first access, `max-3` = from_gen, `max-2` = ws refault, `max-1` = slow refault.
These bins are therefore *not* depth data at the moment. Real overflow samples land in bin `max` (999).

## 3. Status

Commit history in brief: the endpoint and binning were built first. Then came a long hunt for overcounting / double counting, which led to moving the first-access hook out of `lru_gen_add_folio` into `do_anonymous_page` and adding per-path debug counters. Next, the periodic page-walk kthread was added. Most recently came `PG_counted` dedup ("first build with new system").

The HEAD commit's own open items:
- **Decide on the swap-in path.** Should refault accounting stay in `lru_gen_refault` or move into `do_swap_page`?
- **Decide which paths should *not* set the counted bit.**

Uncommitted: `mm/mpc.c` gains page-walk includes and two unused `PAGE_LOGGER_*` defines; `build.log` changed.

### 3.1 Build blockers (from reading the code; I did not compile it)

1. **`mpc_hook_from_gen` signature mismatch.**
   - `include/linux/mpc.h:57` declares `(folio, unsigned long old_gen, unsigned long new_gen, lrugen, memcg)`.
   - `mm/mpc.c:164` defines `(folio, lrugen, memcg, int old_gen)`. The prototypes conflict, so `mpc.c` won't compile.
   - `mm/vmscan.c:4064` passes 4 args (this matches mpc.c).
   - `mm/vmscan.c:4747` (`lru_gen_look_around`) passes **3** args and is missing `old_gen`.
2. **`page_logger_pte_entry`** (`mm/mpc.c:244`) is declared `void`, but `mm_walk_ops.pte_entry` must return `int`. The function also contains `return 0;`.
3. `mpc_log_skip()` is declared in `mpc.h` but never defined. This is harmless until something calls it.
4. Minor: `PAGEFLAG(Counted, counted, PF_NO_HEAD)` and `TEST{SET,CLEAR}FLAG(..., PF_HEAD)` use different policies, and there are declaration-after-statement warnings in `mpc.c`.

### 3.2 Accuracy concerns and open design questions

- **First-access depth = 0.** Under the strict definition ("memory needed to avoid a fault"), a compulsory miss faults at every memory size, so it would belong in the overflow/∞ bin. Recording it at 0 effectively treats zero-fill faults as free. Decide which you want and document it, since it changes how the curve is built. COW (`wp_page_copy`) is also counted as a first access.
- **Swap readahead overcount.** `lru_gen_refault` runs from `workingset_refault` for *every* page read into swap cache, including readahead neighbours that are never accessed. This is likely related to the "swap-in path" TODO. Hooking in `do_swap_page` would count only accesses that actually happen. Swap-cache hits (minor faults on pages still in swap cache) are currently not counted at all.
- **Unconditional `folio_set_active()` in `lru_gen_refault`** changes MGLRU behaviour: every refaulted or readahead folio goes into the youngest generation. Its comment ("prevent double counting") refers to the old `lru_gen_add_folio` hook and may now be stale, since `PG_counted` covers double counting.
- **Stale `PG_counted` leads to undercounting.** If something clears the young bit without calling `mpc_hook_from_gen`, the bit stays set and the *next real* access is silently skipped. Paths that do this include:
  - `walk_pmd_range` / THP PMD-young handling (no hook)
  - look-around on isolated eviction candidates (`old_gen < 0`)
  - other `ptep_clear_young` users
- **Missed samples.** THP/PMD-mapped accesses, and accesses on folios temporarily off-LRU (`old_gen < 0`), are dropped.
- **Depth scope.**
  - Depth uses one node's `lruvec` only (NUMA ignored).
  - Slow-refault depth uses **system-wide** swap usage, not the cgroup's.
  - `nr_pages[][][]` is updated lazily (batched in `walk_pte_range`) and can be briefly stale or negative.
  - `mpc_sum_anon_gens` runs on every sample; a precomputed total was noted as a possible optimisation.
- **"ws" vs "slow" refault naming.** For anon pages both are swap reads. `recent` only means the shadow's eviction seq is still near `min_seq`.
- **kthread cost and design.**
  - One kthread per memcg; systemd creates many cgroups.
  - `mmap_write_lock` on every mm every 30 s. `PGWALK_RDLOCK` / `mmap_read_lock` should be enough.
  - It uses the walking cgroup's `memcg` instead of the folio's own memcg.
  - It skips `lru_gen_look_around`'s pfn/node/memcg sanity checks.
  - The thread starts in `css_alloc`, before the cgroup is online.
- **Counters are 32-bit `atomic_t`.** Watch for overflow on long runs.
- **Parameters.** `memcontrol.c:5389` has a TODO: how should bin parameters be chosen per cgroup?

## 4. Next steps

1. Fix the `mpc_hook_from_gen` prototype and call sites and the `pte_entry` return type, then get a clean build.
2. Decide the swap-in hook location (`lru_gen_refault` vs `do_swap_page`) and whether to keep `folio_set_active`.
3. Audit which paths set or clear `PG_counted` (the HEAD TODO), including young-bit clearers that bypass the hook.
4. Remove the debug counters in bins `max-4..max-1`, or move them to separate fields.
5. Validate against a synthetic workload with a known reuse-distance distribution.
