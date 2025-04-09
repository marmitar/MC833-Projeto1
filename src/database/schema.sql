CREATE TABLE IF NOT EXISTS movie(
    -- Identificador: Número único para cada filme
    id INTEGER PRIMARY KEY NOT NULL DEFAULT (random()),
    -- Título: Nome do filme
    title TEXT NOT NULL,
    -- Diretor(a): Nome do diretor(a)
    director TEXT NOT NULL,
    -- Ano de lançamento: Ano em que foi lançado
    release_year INTEGER NOT NULL
) STRICT, WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS genre(
    id INTEGER PRIMARY KEY NOT NULL DEFAULT (random()),
    -- Gênero: Pode ter um ou mais gêneros
    name TEXT UNIQUE NOT NULL
) STRICT, WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS movie_genre(
    movie_id INTEGER NOT NULL,
    genre_id INTEGER NOT NULL,
    FOREIGN KEY (movie_id) REFERENCES movie(id),
    FOREIGN KEY (genre_id) REFERENCES genre(id),
    UNIQUE (movie_id, genre_id)
) STRICT;

CREATE UNIQUE INDEX IF NOT EXISTS movie_id_1 ON movie(id);
CREATE UNIQUE INDEX IF NOT EXISTS genre_id_1 ON genre(id);
CREATE UNIQUE INDEX IF NOT EXISTS genre_name ON genre(name);
CREATE INDEX IF NOT EXISTS movie_id_2 ON movie_genre(movie_id);
CREATE INDEX IF NOT EXISTS genre_id_2 ON movie_genre(genre_id);

-- INSERT INTO genre(name)
--     VALUES ('Action'), ('Sci-Fi'), ('Thriller');

-- BEGIN TRANSACTION;
-- INSERT INTO movie(title, director, release_year)
--     VALUES ('Inception', 'Christopher Nolan', 2010);
-- INSERT INTO movie_genre(movie_id, genre_id)
--     SELECT movie.id, genre.id
--         FROM movie JOIN genre
--         WHERE movie.title = 'Inception'
--             AND (genre.name = 'Action' OR genre.name = 'Sci-Fi');
-- COMMIT;

-- BEGIN TRANSACTION;
-- INSERT INTO movie(title, director, release_year)
--     VALUES ('Die Hard', 'John McTiernan', 1988);
-- INSERT INTO movie_genre(movie_id, genre_id)
--     SELECT movie.id, genre.id
--         FROM movie JOIN genre
--         WHERE movie.title = 'Die Hard'
--             AND genre.name = 'Action';
-- COMMIT;

-- EXPLAIN QUERY PLAN
-- SELECT genre.name
--     FROM movie_genre
--         INNER JOIN movie ON movie.id = movie_genre.movie_id
--         INNER JOIN genre ON genre.id = movie_genre.genre_id
--     WHERE movie.title = 'Inception';

-- EXPLAIN QUERY PLAN
-- SELECT movie.id, movie.title
--     FROM movie_genre
--         INNER JOIN movie ON movie.id = movie_genre.movie_id
--         INNER JOIN genre ON genre.id = movie_genre.genre_id
--     WHERE genre.name = 'Action';

-- EXPLAIN QUERY PLAN
-- SELECT id, title
--     FROM movie
--     WHERE movie.id IN (
--         SELECT movie_id
--             FROM movie_genre
--             WHERE genre_id = (
--                 SELECT id
--                     FROM genre
--                     WHERE name = 'Action'
--             )
--     );
