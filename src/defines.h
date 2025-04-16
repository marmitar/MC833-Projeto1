#ifndef SRC_DEFINES_H
/** Utility macros. */
#define SRC_DEFINES_H

#if defined(__clang__)
/**
 * Indicates that the pointer is possibly `NULL`.
 * @see https://clang.llvm.org/docs/AttributeReference.html#nullable
 */
#    define NULLABLE _Nullable
/**
 * Indicates that the pointer should never be `NULL`.
 * @see https://clang.llvm.org/docs/AttributeReference.html#nonnull
 */
#    define NONNULL _Nonnull
// For completeness.
#    define UNSPECIFIED _Null_unspecified
#else
// These pointer modifiers are specific to Clang, and are left as comments for readers on GCC.
#    define NULLABLE
#    define NONNULL
#    define UNSPECIFIED
#endif

/**
 * Marker for a branch that should be taken often or should be optimized for. Usually the "happy" case.
 * Probability is assumed to be 90%.
 */
#define likely(x) (__builtin_expect((x), 1))
/**
 * Marker for a branch that should be taken rarely or should be optimized against. Usually the error path.
 * Probability is assumed to be 10%.
 */
#define unlikely(x) (__builtin_expect((x), 0))

/** Stringification macro. */
#define STR(...) STR_(__VA_ARGS__)
// https://gcc.gnu.org/onlinedocs/gcc-4.8.5/cpp/Stringification.html
#define STR_(...) #__VA_ARGS__

/** Happy result, no errors. */
#define OK 0

/** Passes integer as pointer argument for callbacks. */
#define PTR_FROM_INT(x) ((void *) (intptr_t) (x))  // NOLINT(performance-no-int-to-ptr)
/** Recover integer from callback pointer. */
#define INT_FROM_PTR(p) ((int) (intptr_t) (p))

#endif  // SRC_DEFINES_H
