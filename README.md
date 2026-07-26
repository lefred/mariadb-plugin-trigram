# mariadb-plugin-trigram

![mariabd-plugin-trigram](logo/trigram.png)

This native MariaDB Server plugin provides case-insensitive, character-aware
trigram matching. Words are padded with two spaces on the left and one on the
right, following the convention used by PostgreSQL's `pg_trgm`.

Functions:

- `TRIGRAM_SIMILARITY(left, right)` returns the Jaccard similarity of the two
  sets of trigrams, from `0.0` to `1.0`.
- `TRIGRAM_WORD_SIMILARITY(left, right)` returns the greatest similarity
  between the first string and any contiguous trigram extent in the second.
- `TRIGRAM_STRICT_WORD_SIMILARITY(left, right)` restricts the extent to whole
  words. `TRIGRAM_STRICT_WORLD_SIMILARIRY()` is also installed as a
  compatibility alias for the spelling in the original proposal.
- `TRIGRAMS(value)` returns the sorted, distinct trigram set as a JSON array.
- `TRIGRAM_COUNT(value)` returns the number of distinct trigrams.
- `TRIGRAM_DISTANCE(left, right)` returns `1 - TRIGRAM_SIMILARITY(...)`.
- `TRIGRAM_MATCH(left, right, threshold)` returns whether similarity meets a
  threshold from `0.0` to `1.0`.

All functions propagate `NULL`. Matching uses characters in the argument's
MariaDB character set rather than bytes, ignores non-alphanumeric separators,
and folds case according to that character set.

## Comparison with PostgreSQL and MySQL

This plugin's function names and semantics (word padding, Jaccard
similarity, word/strict-word extents) deliberately mirror PostgreSQL's
`pg_trgm` contrib module, since neither MariaDB nor MySQL ship anything
equivalent out of the box.

| | mariadb-plugin-trigram | PostgreSQL `pg_trgm` | MySQL |
|---|---|---|---|
| Similarity score | `TRIGRAM_SIMILARITY()`, `TRIGRAM_WORD_SIMILARITY()`, `TRIGRAM_STRICT_WORD_SIMILARITY()` | `similarity()`, `word_similarity()`, `strict_word_similarity()` | none built-in |
| Distance / threshold | `TRIGRAM_DISTANCE()`, `TRIGRAM_MATCH()` | `<->`/`<<->>`/`<<<->>>` distance operators, `%`/`<%`/`%>` threshold operators, `pg_trgm.similarity_threshold` GUC | none |
| Inspect the trigram set | `TRIGRAMS()`, `TRIGRAM_COUNT()` | `show_trgm()` | none |
| Case folding / charset awareness | Uses the argument's own MariaDB character set for case folding and character iteration | Locale/encoding-aware | n/a |
| Index acceleration | None yet — these are plain functions, so filtering a large table on similarity means a full scan | `gist_trgm_ops` / `gin_trgm_ops` let GiST/GIN indexes accelerate `%`, the distance operators, and even `LIKE`/`ILIKE` | n/a |
| Nearest built-in alternative | — | — | The `ngram` full-text parser (mainly for CJK tokenization) and native `FULLTEXT` relevance ranking; neither produces a similarity/distance score between two arbitrary strings |
| License | GPLv2 | PostgreSQL License (bundled contrib module) | n/a |

The main functional gap compared to `pg_trgm` is indexing: without a GiST/GIN
equivalent, `TRIGRAM_SIMILARITY(col, 'query') > 0.3` will scan every row.
`TRIGRAM_WORD_SIMILARITY()`/`TRIGRAM_STRICT_WORD_SIMILARITY()` are also
O(n²) in the length of their second argument (an incremental sliding-window
algorithm, not `pg_trgm`'s more sophisticated internal one — see
[AI_CHANGES.md](AI_CHANGES.md)), so they are best applied to short-to-medium
text rather than very large columns.

## Build and install

Place or symlink this directory at `plugin/trigram` in a MariaDB Server source
tree, then configure and build:

```sh
cmake -S . -B build -DPLUGIN_TRIGRAM=DYNAMIC
cmake --build build --target trigram
```

Copy the resulting module to MariaDB's plugin directory and run:

```sql
INSTALL SONAME 'trigram';

SELECT TRIGRAM_SIMILARITY('MariaDB', 'Maria DB');
SELECT TRIGRAM_WORD_SIMILARITY('database', 'a fast database server');
SELECT TRIGRAMS('cat');
```

Remove all functions with `UNINSTALL SONAME 'trigram'`.

## Test

Build `mariadbd`, `mariadb-test`, and `trigram`, then from the build tree's
`mysql-test` directory run:

```sh
perl mariadb-test-run.pl --suite=trigram
```
