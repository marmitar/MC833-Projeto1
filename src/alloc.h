#ifndef SRC_ALLOC_H
/** Utilities for aligned allocation. */
#define SRC_ALLOC_H

#include <assert.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "./defines.h"

/** Check that the pointer is aligned correctly after allocation. */
#define is_aligned(alignment, ptr) ((uintptr_t) ptr) % alignment == 0  // NOLINT(bugprone-macro-parentheses)

#define assume_aligned_as(alignment, ptr) __builtin_assume_aligned(ptr, alignment)

#if defined(__clang__)
/**  */
#    define aligned_as(alignment, ptr) assume_aligned_as(alignment, (assume(is_aligned(alignment, ptr)), (ptr)))
#else  // GCC
#    define aligned_as(alignment, ptr) assume_aligned_as(alignment, (assert(is_aligned(alignment, ptr)), (ptr)))
#endif

/** Assume pointer is aligned to `ty`. Checked on DEBUG builds. */
#define aligned_like(ty, ptr) aligned_as(alignof(ty), ptr)

/** Check that `number` is a power of two. Used for alignment. */
#define is_power_of_two(number) (__builtin_popcountg(number) == 1)

[[gnu::malloc, gnu::alloc_align(1), gnu::alloc_size(2, 3), gnu::nothrow, gnu::used]]
/**
 * Custom `aligned_alloc`.
 *
 * Allocates a memory for `count` elements of size `size`, all aligned to `alignment`. This function is always inlined,
 * so the checks are visible on call site, and can be optimized away most of the time, reducing the function call to
 * `aligned_alloc(alignment, count * size)`.
 *
 * Returns `NULL` if: `alignment` is not a power of two, `count * size` overflows, or if allocation fails (`ENOMEM`).
 */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static inline void *NULLABLE alloc_aligned(size_t alignment, size_t count, size_t size) {
    assume(is_power_of_two(alignment));
    assume(size > 0);

    size_t bytes;
    bool overflow = ckd_mul(&bytes, count, size);
    if unlikely (overflow) {
        return NULL;
    }

    assume(bytes % alignment == 0);
    void *NULLABLE ptr = aligned_alloc(alignment, bytes);
    if unlikely (ptr == NULL) {
        return NULL;
    }

    assume(is_aligned(alignment, ptr));
    return ptr;
}

/**
 * Allocate one or more objects of type `ty`, ensuring correct alignment.
 */
#define alloc_like(ty, ...) alloc_like_(ty, __VA_ARGS__ __VA_OPT__(, ) 1)
/** Internal implementation for variadic `alloc_like` that resolves the `count` as input or default of 1. */
#define alloc_like_(ty, count, ...) (ty *) assume_aligned_as(alignof(ty), alloc_aligned(alignof(ty), count, sizeof(ty)))

#endif  // SRC_ALLOC_H
