#ifndef SRC_MOVIE_H
/** Movie data. */
#define SRC_MOVIE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "../defines.h"

/**
 * Represents a single movie record, including an embedded list of genres.
 */
struct movie {  // NOLINT(altera-struct-pack-align)
    /** Unique identifier for the movie entry in the database. */
    int64_t id;  // zero for adding
    /** Movie title. */
    const char *NONNULL title;
    /** Director name. */
    const char *NONNULL director;
    /** Year the movie was released. */
    int release_year;
    /** List of genres for the movie. */
    const char *NONNULL *NONNULL genres;
    /** Size of the `genres` list. */
    size_t genre_count;
};

/**
 * Short representation of a movie.
 */
struct [[gnu::aligned(2 * sizeof(int64_t))]] movie_summary {
    /** Unique identifier for the movie entry in the database. */
    int64_t id;
    /** Embedded movie title. */
    const char *NONNULL title;
};
// even on 32 bit, fields should be aligned to 8 bytes here
static_assert(sizeof(struct movie_summary) == 2 * sizeof(int64_t));

/** Free a single movie. Shared strings not deallocated. */
static inline void free_movie(struct movie movie) {
    free((void *) movie.genres);
}

#endif  // SRC_MOVIE_H
