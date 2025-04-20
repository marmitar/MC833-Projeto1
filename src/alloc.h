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
#define is_aligned_(alignment, ptr) ((uintptr_t) ptr) % alignment == 0  // NOLINT(bugprone-macro-parentheses)

/** Assume pointer is aligned to `ty`. Checked on DEBUG builds. */
#define get_aligned(ty, ptr) __builtin_assume_aligned((assume(is_aligned_(alignof(ty), ptr)), (ptr)), alignof(ty))

/** Check that `number` is a power of two. Used for alignment. */
#define is_power_of_two(number) (__builtin_popcountg(number) == 1)

[[gnu::malloc, gnu::alloc_align(1), gnu::alloc_size(2, 3), gnu::always_inline, gnu::leaf, gnu::nothrow]]
/**
 * `aligned_alloc` + `calloc`.
 *
 * Allocates a memory for `count` elements of size `size`, all aligned to `alignment`. This function is always inlined,
 * so the checks are visible on call site, and can be optimized away most of the time, reducing the function call to
 * `aligned_alloc(alignement, count * size)`.
 *
 * Returns `NULL` if: `alignment` is not a power of two, `count * size` overflows, or if allocation fails (`ENOMEM`).
 */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static inline void *NULLABLE calloc_aligned(size_t alignment, size_t size, size_t count) {
    assert(is_power_of_two(alignment));
    if unlikely (!is_power_of_two(alignment)) {
        return NULL;
    }

    size_t bytes = 0;
    bool overflow = ckd_mul(&bytes, count, size);
    if unlikely (overflow) {
        return NULL;
    }

    void *NULLABLE ptr = aligned_alloc(alignment, bytes);
    if unlikely (ptr == NULL) {
        return NULL;
    }

    assume(is_aligned_(alignment, ptr));
    memset(ptr, 0, bytes);
    return ptr;
}

/**
 * Allocate one or more objects of type `ty`, ensuring correct alignment.
 */
#define calloc_like(ty, ...) calloc_like_(ty, __VA_ARGS__ __VA_OPT__(, ) 1)
/** Internal implementation for variadic `calloc_like` that resolves the `count` as input or default of 1. */
#define calloc_like_(ty, count, ...) \
    (ty *) __builtin_assume_aligned(calloc_aligned(alignof(ty), sizeof(ty), count), alignof(ty))

/**
 * `sizeof` for structs with Flexible Array Members.
 */
#define size_of_fam(ty, last_field, count) (offsetof(ty, last_field) + sizeof(((ty) {}).last_field[0]) * (count))

/**
 * Allocates memore for a struct with Flexible Array Members.
 */
#define calloc_fam(ty, last_field, count) \
    (ty *) __builtin_assume_aligned(calloc_aligned(alignof(ty), 1, size_of_fam(ty, last_field, count)), alignof(ty))

#endif  // SRC_ALLOC_H
