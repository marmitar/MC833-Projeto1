#ifndef SRC_MOVIE_BUILDER_H
/** Movie data creation. */
#define SRC_MOVIE_BUILDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../defines.h"
#include "./movie.h"

/** Optimal alignment for `movie_builder_t`. */
#define ALIGNMENT_MOVIE_BUILDER 128

/**
 * Internal buffer for movie output.
 */
typedef struct movie_builder movie_builder_t [[gnu::aligned(ALIGNMENT_MOVIE_BUILDER)]];

[[nodiscard("must be destroyed"), gnu::malloc, gnu::assume_aligned(ALIGNMENT_MOVIE_BUILDER), gnu::leaf, gnu::nothrow]]
/**
 * Allocates initial memory for a reusable movie builder.
 *
 * Returns the newly allocated builer, or `NULL` on out-of-memory situations.
 */
movie_builder_t *NULLABLE movie_builder_create(void);

[[gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
/**
 * Release memory used for buffer. Cannot be used again.
 */
void movie_builder_destroy(movie_builder_t *NONNULL builder);

[[gnu::nonnull(1), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Reset the internal state in the builder. All previous references cannot be used again.
 *
 * This does not deallocates memory, just resets counters, so the memory can be reused.
 */
void movie_builder_reset(movie_builder_t *NONNULL builder);

[[gnu::nonnull(1), gnu::hot, gnu::pure, gnu::leaf, gnu::nothrow]]
/**
 * Check if `movie_id` is already set for the current movie.
 */
bool movie_builder_has_id(const movie_builder_t *NONNULL builder);

[[gnu::nonnull(1), gnu::hot, gnu::pure, gnu::leaf, gnu::nothrow]]
/**
 * Check if `title` is already set for the current movie.
 */
bool movie_builder_has_title(const movie_builder_t *NONNULL builder);

[[gnu::nonnull(1), gnu::hot, gnu::pure, gnu::leaf, gnu::nothrow]]
/**
 * Check if `director` is already set for the current movie.
 */
bool movie_builder_has_director(const movie_builder_t *NONNULL builder);

[[gnu::nonnull(1), gnu::hot, gnu::pure, gnu::leaf, gnu::nothrow]]
/**
 * Check if `release_year` is already set for the current movie.
 */
bool movie_builder_has_release_year(const movie_builder_t *NONNULL builder);

[[gnu::nonnull(1), gnu::hot, gnu::pure, gnu::leaf, gnu::nothrow]]
/**
 * Check if `genres` is already set for the current movie.
 */
bool movie_builder_has_genres(const movie_builder_t *NONNULL builder);

[[gnu::nonnull(1), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Set the identifier for the current movie.
 *
 * Should not be called more than once for the same movie.
 */
void movie_builder_set_id(movie_builder_t *NONNULL builder, int64_t movie_id);

[[gnu::nonnull(1, 3), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Set the title for the current movie.
 *
 * Should not be called more than once for the same movie.
 *
 * Returns `true` on success and `false` on allocation failures.
 */
bool movie_builder_set_title(movie_builder_t *NONNULL builder, size_t len, const char title[NONNULL restrict len + 1]);

[[gnu::nonnull(1, 3), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Set the director for the current movie.
 *
 * Should not be called more than once for the same movie.
 *
 * Returns `true` on success and `false` on allocation failures.
 */
bool movie_builder_set_director(
    movie_builder_t *NONNULL builder,
    size_t len,
    const char director[NONNULL restrict len + 1]
);

[[gnu::nonnull(1), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Set the release year for the current movie.
 *
 * Should not be called more than once for the same movie.
 */
void movie_builder_set_release_year(movie_builder_t *NONNULL builder, int release_year);

[[gnu::nonnull(1), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Start the genre list for the current movie.
 *
 * Should not be called more than once for the same movie.
 */
void movie_builder_start_genres(movie_builder_t *NONNULL builder);

[[gnu::nonnull(1, 3), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Add genre to the current movie's genres list.
 *
 * Should not be called after `movie_builder_start_genres()` and before other `movie_builder_set_*()` calls.
 *
 * Returns `true` on success and `false` on allocation failures.
 */
bool movie_builder_add_genre(movie_builder_t *NONNULL builder, size_t len, const char genre[NONNULL restrict len + 1]);

[[nodiscard("output may be uninitialized"), gnu::nonnull(1, 2), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Dereference the current movie.
 *
 * The builder should not be modified after this, until the reference is forgotten.
 *
 * Returns `true` on success and `false` on allocation failures.
 */
bool movie_builder_take_current_movie(const movie_builder_t *NONNULL builder, struct movie *NONNULL output);

[[gnu::nonnull(1, 2), gnu::leaf, gnu::nothrow]]
/**
 * Dereference the summary of the current movie.
 *
 * The builder should not be modified after this, until the reference is forgotten.
 */
void movie_builder_take_current_summary(const movie_builder_t *NONNULL builder, struct movie_summary *NONNULL output);

[[nodiscard("must be freed"), gnu::nonnull(1, 2), gnu::hot, gnu::malloc, gnu::leaf, gnu::nothrow]]
/**
 * Dereference the genre list of the current movie.
 *
 * The builder should not be modified after this, until the reference is forgotten.
 *
 * Returns the list of `length` genres on success, and `NULL` on allocation failures.
 */
const char *NONNULL *NULLABLE
    movie_builder_take_current_genres(const movie_builder_t *NONNULL builder, size_t *NONNULL length);

[[gnu::nonnull(1), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Adds current build data as `struct movie` into the build list. This automatically resets the builder data.
 *
 * All the data must have been set correctly by previous calls to `movie_builder_set_*()` functions.
 *
 * Returns `true` on success and `false` on allocation failures.
 */
bool movie_builder_add_current_movie_to_list(movie_builder_t *NONNULL builder);

[[gnu::nonnull(1), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Adds current build data as `struct movie_summary` into the build list. This automatically resets the builder data.
 *
 * The data must have been set correctly by previous calls to `movie_builder_set_id()` and `movie_builder_set_title()`
 * functions.
 *
 * Returns `true` on success and `false` on allocation failures.
 */
bool movie_builder_add_current_summary_to_list(movie_builder_t *NONNULL builder);

[[nodiscard("useless call if discarded"), gnu::pure, gnu::nonnull(1), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Current number of movies in list.
 */
size_t movie_builder_list_size(const movie_builder_t *NONNULL builder);

[[gnu::nonnull(1), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Dereference the movie from the current list.
 *
 * The builder should not be modified after this, until the reference is forgotten.
 *
 * Returns `true` on success and `false` on allocation failures.
 */
bool movie_builder_take_movie_from_list(
    const movie_builder_t *NONNULL builder,
    size_t idx,
    struct movie *NONNULL output
);

[[nodiscard("must be freed"), gnu::nonnull(1, 2), gnu::malloc, gnu::leaf, gnu::nothrow]]
/**
 * Dereference the entire list of movies.
 *
 * The builder should not be modified after this, until the references are destroyed.
 *
 * If an internal allocation occurs while the movie list is already allocated, then an incomplete list is returned and
 * no error is set.
 *
 * Returns the list of `count` movies on success, and `NULL` on allocation failures.
 */
struct movie *NULLABLE movie_builder_take_movie_list(const movie_builder_t *NONNULL builder, size_t *NONNULL count);

[[nodiscard("must be freed"), gnu::nonnull(1, 2), gnu::malloc, gnu::leaf, gnu::nothrow]]
/**
 * Dereference the entire list of summaries.
 *
 * The builder should not be modified after this, until the references are destroyed.
 *
 * Returns the list of `count` summaries on success, and `NULL` on allocation failures.
 */
struct movie_summary *NULLABLE
    movie_builder_take_summary_list(const movie_builder_t *NONNULL builder, size_t *NONNULL count);

#endif  // SRC_MOVIE_BUILDER_H
