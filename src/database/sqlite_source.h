// IWYU pragma: private
// IWYU pragma: no_include
// IWYU pragma: friend "*database.c"
// IWYU pragma: always_keep

#ifndef SRC_DATABASE_SQLITE_SOURCE_H
#define SRC_DATABASE_SQLITE_SOURCE_H

// Includes the full SQLite 3 source code amalgamation for aggressive optimizations.
// Disable some diagnostic messages from that file, as they are not part of this source code.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Rpass"
#pragma clang diagnostic ignored "-Rpass-missed"
#pragma clang diagnostic ignored "-Wpragma-system-header-outside-header"

#pragma GCC system_header
// IWYU pragma: begin_exports
#include <sqlite3.c>  // NOLINT(bugprone-suspicious-include)
// IWYU pragma: end_exports

// Remove some left-overs from the amalgamation.
#undef likely
#undef unlikely

#pragma clang diagnostic pop

#endif  // SRC_DATABASE_SQLITE_SOURCE_H
