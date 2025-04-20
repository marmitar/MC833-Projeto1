#ifndef SRC_DATABASE_SCHEMA_H
#define SRC_DATABASE_SCHEMA_H

// clang-format off
static constexpr const char SCHEMA[] =
    "CREATE TABLE IF NOT EXISTS movie(\n"
    "    -- Identificador: Número único para cada filme\n"
    "    id INTEGER PRIMARY KEY ASC AUTOINCREMENT NOT NULL,\n"
    "    -- Título: Nome do filme\n"
    "    title TEXT NOT NULL,\n"
    "    -- Diretor(a): Nome do diretor(a)\n"
    "    director TEXT NOT NULL,\n"
    "    -- Ano de lançamento: Ano em que foi lançado\n"
    "    release_year INTEGER NOT NULL\n"
    ") STRICT;\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS genre(\n"
    "    id INTEGER PRIMARY KEY ASC AUTOINCREMENT NOT NULL,\n"
    "    -- Gênero: Pode ter um ou mais gêneros\n"
    "    name TEXT UNIQUE NOT NULL\n"
    ") STRICT;\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS movie_genre(\n"
    "    movie_id INTEGER NOT NULL,\n"
    "    genre_id INTEGER NOT NULL,\n"
    "    FOREIGN KEY (movie_id)\n"
    "        REFERENCES movie(id)\n"
    "        ON DELETE CASCADE,\n"
    "    FOREIGN KEY (genre_id)\n"
    "        REFERENCES genre(id)\n"
    "        ON DELETE CASCADE,\n"
    "    UNIQUE (movie_id, genre_id)\n"
    ") STRICT;\n"
    "\n"
    "CREATE UNIQUE INDEX IF NOT EXISTS genre_name ON genre(name);\n"
    "CREATE INDEX IF NOT EXISTS movie_id_link ON movie_genre(movie_id);\n"
    "CREATE INDEX IF NOT EXISTS genre_id_link ON movie_genre(genre_id);\n"
;
// clang-format on

#endif  // SRC_DATABASE_SCHEMA_H
