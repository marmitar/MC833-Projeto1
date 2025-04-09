// IWYU pragma: private
// IWYU pragma: no_include

// Includes the full SQLite 3 source code amalgamation for aggressive optimizations.
// Disable some diagnostic messages from that file, as they are not part of this source code.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Rpass"
#pragma clang diagnostic ignored "-Rpass-missed"

#pragma GCC system_header
#include <sqlite3.c>  // NOLINT(bugprone-suspicious-include)

// Remove some left-overs from the amalgamation.
#undef likely
#undef unlikely

#pragma clang diagnostic pop
