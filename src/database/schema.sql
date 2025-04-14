CREATE TABLE IF NOT EXISTS movie(
    -- Identificador: Número único para cada filme
    id INTEGER PRIMARY KEY ASC AUTOINCREMENT NOT NULL,
    -- Título: Nome do filme
    title TEXT NOT NULL,
    -- Diretor(a): Nome do diretor(a)
    director TEXT NOT NULL,
    -- Ano de lançamento: Ano em que foi lançado
    release_year INTEGER NOT NULL
) STRICT;

CREATE TABLE IF NOT EXISTS genre(
    id INTEGER PRIMARY KEY ASC AUTOINCREMENT NOT NULL,
    -- Gênero: Pode ter um ou mais gêneros
    name TEXT UNIQUE NOT NULL
) STRICT;

CREATE TABLE IF NOT EXISTS movie_genre(
    movie_id INTEGER NOT NULL,
    genre_id INTEGER NOT NULL,
    FOREIGN KEY (movie_id)
        REFERENCES movie(id)
        ON DELETE CASCADE,
    FOREIGN KEY (genre_id)
        REFERENCES genre(id)
        ON DELETE CASCADE,
    UNIQUE (movie_id, genre_id)
) STRICT;

CREATE UNIQUE INDEX IF NOT EXISTS genre_name ON genre(name);
CREATE INDEX IF NOT EXISTS movie_id_link ON movie_genre(movie_id);
CREATE INDEX IF NOT EXISTS genre_id_link ON movie_genre(genre_id);
