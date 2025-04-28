#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../alloc.h"
#include "../defines.h"
#include "./builder.h"
#include "./movie.h"

/**
 * Movie using string references inside `movie_builder_t`.
 *
 * The references are valid in between calls to `movie_builder_reset()`. Once dereferenced, the `movie_builder_t`
 * should not be mutated anymore, until all references are dropped.
 */
struct movie_ref {  // NOLINT(altera-struct-pack-align)
    /** The actual id. */
    int64_t movie_id;
    /** A reference to the title slice in `movie_builder_t`. */
    size_t title_slice;
    /** A reference to the director slice in `movie_builder_t`. */
    size_t director_slice;
    /** The actual release year. */
    int release_year;
    /** A reference to the genres slices in `movie_builder_t`. */
    size_t genres_slice;
    /** How many genres are in `genres_slice`. */
    size_t genres_count;
};

/**
 * Internal buffer for movie output.
 *
 * This holds an in progress `current` movie, alognside its current build status. Once completed, the movie is either
 * dereferenced into a `struct movie` or `struct movie_summary`, and the build cannot be modified anymore. Or the
 * reference is added to `movie_list` and a new one is started, so they can all be derenferenced at once later on.
 *
 * The builder keeps a single mutable string allocation in `str_data`, where all strings live. These string slices can
 * be modified, but not resized. Once the slices are finally dereferenced, the builder enters read-only mode, and should
 * not be mutated, until all references are forgotten and a `movie_builder_reset()` is called.
 */
struct [[gnu::aligned(ALIGNMENT_MOVIE_BUILDER)]] movie_builder {
    /** Currently in progress `struct movie` or `struct movie_summary`. */
    struct movie_ref current;

    /** If `current.movie_id` was set. */
    bool has_id;
    /** If `current.tile_slice` was set. */
    bool has_title;
    /** If `current.director_slice` was set. */
    bool has_director;
    /** If `current.release_year` was set. */
    bool has_release_year;
    /** If `current.genres_slice` was set. */
    bool has_genres;

    /** Modifiable shared string. */
    char *NONNULL restrict str_data;
    /** Current allocated size for `str_data`. */
    size_t str_capacity;
    /** Currently in use part of the string buffer `str_data`. */
    size_t str_in_use;

    /** In progress list of `struct movie` or a list of `struct movie_summary`. */
    struct movie_ref *NONNULL restrict movie_list;
    /** Allocated size of `movie_list`. */
    size_t list_capacity;
    /** Currently in use part of `movie_list`. */
    size_t list_size;
};

/** The step size for each allocation of `movie_builder_t.str_data`. */
#define BUFFER_PAGE_SIZE 4096

/** The step size for each allocation of `movie_builder_t.movie_list`. */
#define MOVIE_LIST_CAPACITY_STEP 128

/** Allocates initial memory for the reusable movie builder. */
movie_builder_t *NULLABLE movie_builder_create(void) {
    movie_builder_t *builder = alloc_like(struct movie_builder);
    if unlikely (builder == NULL) {
        return NULL;
    }

    [[gnu::aligned(BUFFER_PAGE_SIZE)]]
    char *data = alloc_aligned(BUFFER_PAGE_SIZE, BUFFER_PAGE_SIZE, sizeof(char));
    if unlikely (data == NULL) {
        free(builder);
        return NULL;
    }

    struct movie_ref *list = alloc_like(struct movie_ref, 1);
    if unlikely (list == NULL) {
        free(data);
        free(builder);
        return NULL;
    }

    builder->str_data = aligned_as(BUFFER_PAGE_SIZE, data);
    builder->str_capacity = BUFFER_PAGE_SIZE;

    builder->movie_list = list;
    builder->list_capacity = 1;
    return builder;
}

/** Release memory used for builder. */
void movie_builder_destroy(movie_builder_t *NONNULL builder) {
    free(builder->movie_list);
    free(builder->str_data);
    free(builder);
}

/** Reset the buffer to no data a inital state. */
void movie_builder_reset(movie_builder_t *NONNULL builder) {
    builder->str_in_use = 0;
    builder->list_size = 0;

    builder->has_id = false;
    builder->has_title = false;
    builder->has_director = false;
    builder->has_release_year = false;
    builder->has_genres = false;
}

/** Check if `movie_id` is already set for the current movie. */
bool movie_builder_has_id(const movie_builder_t *NONNULL builder) {
    return builder->has_id;
}

/** Check if `title` is already set for the current movie. */
bool movie_builder_has_title(const movie_builder_t *NONNULL builder) {
    return builder->has_title;
}

/** Check if `director` is already set for the current movie. */
bool movie_builder_has_director(const movie_builder_t *NONNULL builder) {
    return builder->has_director;
}

/** Check if `release_year` is already set for the current movie. */
bool movie_builder_has_release_year(const movie_builder_t *NONNULL builder) {
    return builder->has_release_year;
}

/** Check if `genres` is already set for the current movie. */
bool movie_builder_has_genres(const movie_builder_t *NONNULL builder) {
    return builder->has_genres;
}

[[gnu::const]]
/** Calculates $ceil(a / b)$. */
static inline size_t ceil_div(size_t a, size_t b) {
    assume(a != 0);
    assume(b != 0);
    // only works for positive integers
    return 1 + ((a - 1) / b);
}

[[gnu::nonnull(1)]]
/**
 * Reallocates the `str_data` buffer to hold `additional_size` more bytes.
 */
static bool movie_builder_realloc_str_data(movie_builder_t *NONNULL builder, size_t additional_size) {
    // assume that we don't already have capacity for `additional_size`
    assume(builder->str_capacity >= builder->str_in_use);
    assume(builder->str_capacity - builder->str_in_use < additional_size);

    size_t final_size;
    if unlikely (ckd_add(&final_size, builder->str_in_use, additional_size)) {
        return false;
    }
    assume(final_size > builder->str_in_use);
    const size_t final_capacity = ceil_div(final_size, BUFFER_PAGE_SIZE) * BUFFER_PAGE_SIZE;

    [[gnu::aligned(BUFFER_PAGE_SIZE)]]
    char *data = alloc_aligned(BUFFER_PAGE_SIZE, final_capacity, sizeof(char));
    if unlikely (data == NULL) {
        return false;
    };

    memcpy(
        aligned_as(BUFFER_PAGE_SIZE, data),
        aligned_as(BUFFER_PAGE_SIZE, builder->str_data),
        builder->str_in_use * sizeof(char)
    );
    free(builder->str_data);

    builder->str_data = aligned_as(BUFFER_PAGE_SIZE, data);
    builder->str_capacity = final_capacity;
    return true;
}

[[gnu::nonnull(1, 3), gnu::hot]]
/**
 * Request an allocated slice in string buffer for `size + 1` bytes.
 *
 * Returns `true` if the operation was completed successfully, and the requested slice index is written to `slice`.
 * Otherwise, `false` is returned.
 */
static bool movie_builder_create_slice(movie_builder_t *NONNULL builder, size_t size, size_t *NONNULL slice) {

    assume(builder->str_capacity >= builder->str_in_use);
    if unlikely (size > builder->str_capacity - builder->str_in_use) {
        bool ok = movie_builder_realloc_str_data(builder, size);
        if unlikely (!ok) {
            return false;
        }
    }

    *slice = builder->str_in_use;
    builder->str_in_use += size;
    return true;
}

[[gnu::pure, gnu::hot, gnu::nonnull(1)]]
/** Derenference slice into a string pointer. */
static inline char *NONNULL movie_builder_get_str(movie_builder_t *NONNULL builder, size_t slice) {
    assume(slice <= builder->str_in_use);

    [[gnu::aligned(BUFFER_PAGE_SIZE)]]
    char *NONNULL data = aligned_as(BUFFER_PAGE_SIZE, builder->str_data);

    return &(data[slice]);
}

[[gnu::pure, gnu::hot, gnu::nonnull(1)]]
/** Derenference slice into a const string pointer. */
static inline const char *NONNULL movie_builder_get_const_str(const movie_builder_t *NONNULL builder, size_t slice) {
    assume(slice <= builder->str_in_use);

    [[gnu::aligned(BUFFER_PAGE_SIZE)]]
    const char *NONNULL data = aligned_as(BUFFER_PAGE_SIZE, builder->str_data);

    return &(data[slice]);
}

[[gnu::nonnull(1, 3, 4), gnu::hot]]
/**
 * Insert a string `str` of length `len` into the buffer and write the allocated slice in `slice`.
 *
 * Returns `true` on success, or `false` on out-of-memory.
 */
static bool movie_builder_add_string(
    movie_builder_t *NONNULL builder,
    size_t len,
    const char str[NONNULL restrict len + 1],
    size_t *NONNULL slice
) {
    assume(len == strlen(str));

    size_t idx;
    bool ok = movie_builder_create_slice(builder, len + 1, &idx);
    if unlikely (!ok) {
        return false;
    }

    memcpy(movie_builder_get_str(builder, idx), str, len + 1);
    *slice = idx;
    return true;
}

/* Set the identifier for the current movie. */
void movie_builder_set_id(movie_builder_t *NONNULL builder, int64_t movie_id) {
    assume(!builder->has_id);

    builder->current.movie_id = movie_id;
    builder->has_id = true;
}

/* Set the title for the current movie. */
bool movie_builder_set_title(movie_builder_t *NONNULL builder, size_t len, const char title[NONNULL restrict len + 1]) {
    assume(!builder->has_title);

    bool ok = movie_builder_add_string(builder, len, title, &(builder->current.title_slice));
    if unlikely (!ok) {
        return false;
    }

    builder->has_title = true;
    return true;
}

/* Set the director for the current movie. */
bool movie_builder_set_director(
    movie_builder_t *NONNULL builder,
    size_t len,
    const char director[NONNULL restrict len + 1]
) {
    assume(!builder->has_director);

    bool ok = movie_builder_add_string(builder, len, director, &(builder->current.director_slice));
    if unlikely (!ok) {
        return false;
    }

    builder->has_director = true;
    return true;
}

/* Set the release year for the current movie. */
void movie_builder_set_release_year(movie_builder_t *NONNULL builder, int release_year) {
    assume(!builder->has_release_year);

    builder->current.release_year = release_year;
    builder->has_release_year = true;
}

/* Start the genre list for the current movie. */
void movie_builder_start_genres(movie_builder_t *NONNULL builder) {
    assume(!builder->has_genres);

    builder->current.genres_slice = builder->str_in_use;
    builder->current.genres_count = 0;
    builder->has_genres = true;
}

/** Add genre to the current movie's genres list. */
bool movie_builder_add_genre(movie_builder_t *NONNULL builder, size_t len, const char genre[NONNULL restrict len + 1]) {
    assume(builder->has_genres);

    size_t idx;
    bool ok = movie_builder_add_string(builder, len, genre, &idx);
    if unlikely (!ok) {
        return false;
    }

    assert(idx >= builder->current.genres_slice + builder->current.genres_count);
    builder->current.genres_count += 1;
    return true;
}

[[gnu::nonnull(1), gnu::hot, gnu::malloc]]
/**
 * Extract the genre list from a movie reference.
 */
static const char *NONNULL *NULLABLE
    movie_builder_take_genres(const movie_builder_t *NONNULL builder, struct movie_ref ref) {
    const size_t count = ref.genres_count;
    size_t bytes;
    if unlikely (ckd_mul(&bytes, count, sizeof(char *))) {
        return NULL;
    }

    const char *NONNULL *output = (const char **) malloc(bytes);
    if unlikely (output == NULL) {
        return NULL;
    }

    const char *genre = movie_builder_get_const_str(builder, ref.genres_slice);
    for (size_t i = 0; i < count; i++) {
        output[i] = genre;
        // move to the next string in buffer
        genre += strlen(genre) + 1;
    }

    return output;
}

[[gnu::nonnull(1, 3), gnu::hot]]
/**
 * Extract the movie from a reference.
 */
static bool movie_builder_take_movie(
    const movie_builder_t *NONNULL builder,
    struct movie_ref ref,
    struct movie *NONNULL output
) {
    const char *NONNULL *genres = movie_builder_take_genres(builder, ref);
    if unlikely (genres == NULL) {
        return false;
    }

    *output = (struct movie) {
        .id = ref.movie_id,
        .title = movie_builder_get_const_str(builder, ref.title_slice),
        .director = movie_builder_get_const_str(builder, ref.director_slice),
        .release_year = ref.release_year,
        .genres = genres,
        .genre_count = ref.genres_count,
    };
    return true;
}

[[gnu::nonnull(1, 3), gnu::hot]]
/**
 * Extract the movie summary from a reference.
 */
static void movie_builder_take_summary(
    const movie_builder_t *NONNULL builder,
    struct movie_ref ref,
    struct movie_summary *NONNULL output
) {
    *output = (struct movie_summary) {
        .id = ref.movie_id,
        .title = movie_builder_get_const_str(builder, ref.title_slice),
    };
}

/** Dereference the current movie. */
bool movie_builder_take_current_movie(const movie_builder_t *NONNULL builder, struct movie *NONNULL output) {
    assume(builder->has_id);
    assume(builder->has_title);
    assume(builder->has_director);
    assume(builder->has_release_year);
    assume(builder->has_genres);

    return movie_builder_take_movie(builder, builder->current, output);
}

/** Dereference the summary of the current movie. */
void movie_builder_take_current_summary(const movie_builder_t *NONNULL builder, struct movie_summary *NONNULL output) {
    assume(builder->has_id);
    assume(builder->has_title);

    movie_builder_take_summary(builder, builder->current, output);
}

/** Dereference the genre list of the current movie. */
const char *NONNULL *NULLABLE
    movie_builder_take_current_genres(const movie_builder_t *NONNULL builder, size_t *NONNULL length) {
    assume(builder->has_genres);

    const char *NONNULL *genres = movie_builder_take_genres(builder, builder->current);
    if likely (genres != NULL) {
        *length = builder->current.genres_count;
    }
    return genres;
}

[[gnu::nonnull(1)]]
/**
 * Reallocates the `movie_list` buffer to hold more `MOVIE_LIST_CAPACITY_STEP` items.
 */
static bool movie_builder_realloc_movie_list(movie_builder_t *NONNULL builder) {
    assume(builder->list_capacity == builder->list_size);
    assume(builder->list_capacity % MOVIE_LIST_CAPACITY_STEP == 0);

    const size_t current_capacity = builder->list_capacity;
    size_t final_capacity;
    if unlikely (ckd_add(&final_capacity, current_capacity, MOVIE_LIST_CAPACITY_STEP)) {
        return false;
    }

    struct movie_ref *list = alloc_like(struct movie_ref, final_capacity);
    if unlikely (list == NULL) {
        return false;
    }

    memcpy(list, aligned_like(struct movie_ref, builder->movie_list), current_capacity * sizeof(struct movie_ref));
    free(builder->movie_list);

    builder->movie_list = list;
    builder->list_capacity = final_capacity;
    return true;
}

[[gnu::nonnull(1), gnu::hot]]
/**
 * Insert a reference in the `movie_list`.
 *
 * Returns `true` on success and `false` on allocation failures.
 */
static bool movie_build_add_to_list(movie_builder_t *NONNULL builder, struct movie_ref ref) {
    assume(builder->list_capacity >= builder->list_size);
    if unlikely (builder->list_capacity <= builder->list_size) {
        bool ok = movie_builder_realloc_movie_list(builder);
        if unlikely (!ok) {
            return false;
        }
    }

    struct movie_ref *NONNULL list = aligned_like(struct movie_ref, builder->movie_list);
    list[builder->list_size++] = ref;
    return true;
}

/** Adds current build data as `struct movie` into the build list. */
bool movie_builder_add_current_movie_to_list(movie_builder_t *NONNULL builder) {
    assume(builder->has_id);
    assume(builder->has_title);
    assume(builder->has_director);
    assume(builder->has_release_year);
    assume(builder->has_genres);

    bool ok = movie_build_add_to_list(builder, builder->current);
    if unlikely (!ok) {
        return false;
    }

    builder->has_id = false;
    builder->has_title = false;
    builder->has_director = false;
    builder->has_release_year = false;
    builder->has_genres = false;

    return true;
}

/** Adds current build data as `struct movie_summary` into the build list. */
bool movie_builder_add_current_summary_to_list(movie_builder_t *NONNULL builder) {
    assume(builder->has_id);
    assume(builder->has_title);
    assume(!builder->has_director);
    assume(!builder->has_release_year);
    assume(!builder->has_genres);

    struct movie_ref summary = {
        .movie_id = builder->current.movie_id,
        .title_slice = builder->current.title_slice,
        .director_slice = SIZE_MAX,
        .release_year = INT_MAX,
        .genres_slice = SIZE_MAX,
        .genres_count = SIZE_MAX,
    };

    bool ok = movie_build_add_to_list(builder, summary);
    if unlikely (!ok) {
        return false;
    }

    builder->has_id = false;
    builder->has_title = false;

    return true;
}

/** Current number of movies in list. */
size_t movie_builder_list_size(const movie_builder_t *NONNULL builder) {
    return builder->list_size;
}

/** Dereference the movie from the current list. */
bool movie_builder_take_movie_from_list(
    const movie_builder_t *NONNULL builder,
    size_t idx,
    struct movie *NONNULL output
) {
    assume(idx < builder->list_size);

    return movie_builder_take_movie(builder, builder->movie_list[idx], output);
}

/** Dereference the entire list of movies. */
struct movie *NULLABLE movie_builder_take_movie_list(const movie_builder_t *NONNULL builder, size_t *NONNULL count) {
    const size_t list_size = builder->list_size;
    const struct movie_ref *NONNULL refs = aligned_like(struct movie_ref, builder->movie_list);

    struct movie *output = alloc_like(struct movie, list_size);
    if unlikely (output == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < list_size; i++) {
        bool ok = movie_builder_take_movie(builder, refs[i], &(output[i]));
        if unlikely (!ok) {
            *count = i;
            return output;
        }
    }

    *count = list_size;
    return output;
}

/** Dereference the entire list of summaries. */
struct movie_summary *NULLABLE
    movie_builder_take_summary_list(const movie_builder_t *NONNULL builder, size_t *NONNULL count) {
    const size_t list_size = builder->list_size;
    const struct movie_ref *NONNULL refs = aligned_like(struct movie_ref, builder->movie_list);

    struct movie_summary *output = alloc_like(struct movie_summary, list_size);
    if unlikely (output == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < list_size; i++) {
        movie_builder_take_summary(builder, refs[i], &(output[i]));
    }

    *count = list_size;
    return output;
}
