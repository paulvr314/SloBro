# MPC swap-generation tracking

## Purpose

MPC records how long an anonymous folio has been assigned swap space before it
is faulted back in. The value is an approximate generation age, not a
wall-clock timestamp or an MGLRU generation index. It is used to group refaults
by their time in the swap lifecycle.

The state is associated with a swap slot rather than the folio. A folio can be
freed and its physical page reused while its swap entry remains live, so
folio flags and `page_ext` are not reliable storage for this interval.

## Changes

- Each active swap area has a `u32` generation array and a validity bitmap,
  indexed by swap-slot offset ([`swap_info_struct`](../../include/linux/swap.h)).
- The arrays are zero-initialized during `swapon`; allocation failure aborts
  setup and releases any partial allocation. They are released on `swapoff`.
  Reused slots have their validity bit cleared when allocated
  ([`swapfile.c`](../../mm/swapfile.c)).
- After an MPC-tracked folio is added to the swap cache, its current generation
  is written to each swap slot occupied by that folio
  ([`swap_state.c`](../../mm/swap_state.c), [`mpc.c`](../../mm/mpc.c)).
- On a swap fault, the generation is read before `swap_free()` can release the
  slot, then passed to the MPC refault hook
  ([`memory.c`](../../mm/memory.c)).
- Refault ages are accumulated in a per-MPC-endpoint histogram and exported
  with the existing MPC output. The default has five logarithmic bins:
  `0`, `1`, `2–3`, `4–7`, and `8+` generations.

The metadata costs approximately 4 bytes plus 1 validity bit per swap slot.
For 1 GiB of swap using 4 KiB slots, that is about 1 MiB for generations and
32 KiB for the bitmap.

## Algorithm

The current generation is derived from monotonic `jiffies` in intervals of
`PAGE_WALK_INTERVAL_MS` (currently 5 seconds):

```text
current_generation = floor(monotonic_jiffies / jiffies_per_interval)
```

This avoids a separate timer or global counter to initialize and tear down,
and avoids using an MGLRU array index that is recycled. The stored `u32`
generation naturally wraps; unsigned subtraction gives the elapsed generation
count as long as a slot is not retained for a full `u32` cycle.

For an MPC-enabled anonymous folio:

1. When swap space is successfully assigned, store `current_generation` for
   each slot in the folio's swap range and mark those slots valid.
2. Before consuming a swap entry on fault-in, read its saved generation while
   the entry is still live.
3. If a valid generation was found, calculate
   `age = current_generation - saved_generation`.
4. Put the age in a logarithmic bucket. Age zero is bucket 0; otherwise the
   bucket index is `ilog2(age) + 1`, clamped to the last configured bucket.
5. Increment that cgroup's bucket count. Enabling MPC resets the histogram
   along with its other counters.

If MPC was not tracking the folio when its swap slot was assigned, or no valid
generation is present, the refault still follows the existing MPC path but
does not contribute to the swap-age histogram.

## Timing limitation

The generation is recorded when the folio is added to the swap cache. That
can precede the point at which its physical memory is actually reclaimed, so
the reported age can include time spent resident in the swap cache. This is
an intentional coarse proxy; measuring time strictly out of RAM would require
stamping at a later eviction point.

## Validation

The affected kernel objects compiled with `W=1` using the x86 configuration:
`mm/swapfile.o`, `mm/swap_state.o`, `mm/memory.o`, and `mm/mpc.o`.
This was a compile check only; swap-in/swap-off behavior has not been
runtime-tested.
