#ifndef SRC_DATABASE_SCHEMA_H
#define SRC_DATABASE_SCHEMA_H

static constexpr const char SCHEMA[] = {
#ifndef _CLANGD
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wexcess-initializers"
#    pragma clang diagnostic ignored "-Wint-conversion"
#    embed "schema.sql"  // NOLINT(performance-no-int-to-ptr)
#    pragma clang diagnostic pop
    ,
#endif
    '\0'
};

#endif  // SRC_DATABASE_SCHEMA_H
