# AI-assisted changes

This file documents corrections made to `trigram.cc` by an AI coding
assistant after a code review surfaced two bugs, plus the data used to
verify each fix and the model that performed the work.

## Model

- **Claude Sonnet 5** (`claude-sonnet-5`), via Claude Code.
- Date: 2026-07-26.
- Method: read the source, then built and ran the plugin against a real
  MariaDB 13.1.0 server (compiled from the `MariaDB-server` tree this
  plugin is symlinked into) to reproduce each bug empirically before and
  after the fix, rather than relying on static reading alone.

## 1. Word-boundary detection was wrong for non-Latin-1 text

**File/function:** `is_word_character()`

**Before:**
```cc
static bool is_word_character(const CHARSET_INFO *charset, Character character)
{
  return character > 255 || my_isalnum(charset, character);
}
```
Any character above code point 255 was unconditionally treated as a word
character, regardless of its real Unicode category. `my_isalnum()` was not
usable directly for those because it indexes a charset's ctype table with
`(uchar) c`, silently truncating any wide code point to its low byte.

**After:**
```cc
static bool is_word_character(const CHARSET_INFO *charset, Character character)
{
  if (character <= 255)
    return my_isalnum(charset, character);
  if (character > 0xFFFF)
    return false;
  const MY_UNI_CTYPE &page= my_uni_ctype[character >> 8];
  const int ctype= page.ctype ? page.ctype[character & 0xFF] : page.pctype;
  return (ctype & (_MY_U | _MY_L | _MY_NMR)) != 0;
}
```
`my_uni_ctype` is the server's own generic (charset-independent) Unicode
ctype table — confirmed exported from `mariadbd` (`nm -D sql/mariadbd | grep
my_uni_ctype` → `D my_uni_ctype`) and usable from a dynamically loaded
plugin. The lookup pattern (`page.ctype ? page.ctype[...] : page.pctype`)
mirrors the server's own `my_mb_ctype_mb()` in `strings/ctype-mb.c`, which
also treats code points above the Basic Multilingual Plane (> 0xFFFF, e.g.
most emoji) as unclassified/non-word, for consistency with how the rest of
the server categorizes wide characters.

### Data: before / after

Query: `SELECT TRIGRAMS(_utf8mb4'a、b！c');`
(`、` U+3001 IDEOGRAPHIC COMMA and `！` U+FF01 FULLWIDTH EXCLAMATION MARK are
punctuation, not letters.)

| | Result |
|---|---|
| Before | `["  a"," a、","a、b","b！c","、b！","！c "]` — the whole string was treated as **one unbroken word**, spanning across the punctuation. |
| After | `["  a","  b","  c"," a "," b "," c "]` — correctly split into **three words** (`a`, `b`, `c`) at the punctuation, matching README.md's documented behavior ("ignores non-alphanumeric separators"). |

Regression check, plain ASCII (unaffected code path, `character <= 255`):
`SELECT TRIGRAMS('cat, dog!');` → unchanged:
`["  c","  d"," ca"," do","at ","cat","dog","og "]`.

## 2. `TRIGRAM_WORD_SIMILARITY` / `TRIGRAM_STRICT_WORD_SIMILARITY` had cubic-ish time complexity

**Root cause:** the extent-search loops evaluated similarity for every
candidate `(first, last)` sub-range of the right-hand argument's trigram
sequence, and for **each** candidate independently copied, sorted, and
deduplicated that sub-range (`unique_trigrams()`) before scoring it. That's
O(n²) candidate extents × O(n log n) per extent for `TRIGRAM_WORD_SIMILARITY`
(n = trigram count), and a similar blow-up bounded by word count for
`TRIGRAM_STRICT_WORD_SIMILARITY`. Since `n` is driven directly by the length
of a user-supplied string, this was a straightforward CPU-exhaustion vector
reachable by anyone with `SELECT` privilege.

**Fix:** added `best_extent_similarity()`, which processes candidate
extents as an incremental sliding window instead of rebuilding each one from
scratch. For a fixed starting unit, growing the window by one unit at a
time only requires updating running counts (distinct trigrams in the window,
and how many are also in the left-hand set) via a hash map — O(1) amortized
per newly-added trigram — instead of a full re-sort. This turns the
algorithm into O(units × n) instead of O(units² × n log n). The same helper
is parameterized over "units" so it serves both functions: one trigram per
unit for `TRIGRAM_WORD_SIMILARITY`, one whole word per unit for
`TRIGRAM_STRICT_WORD_SIMILARITY` (words are contiguous in the trigram array,
so a word's trigrams can be added to the window in one range-copy per step).

This is **not** a full algorithmic parity with PostgreSQL's `pg_trgm`
internal implementation (which achieves better than quadratic via a more
involved technique) — it is an honest complexity reduction from roughly
cubic to quadratic, achieved without changing the function's documented
semantics or output values. For very large right-hand arguments (tens of
thousands of characters) it will still be slow; it is no longer
pathologically so for realistic inputs (query strings against
paragraph/article-length text).

### Data: before / after (same machine, same build)

`SELECT TRIGRAM_WORD_SIMILARITY('word', @s)` where `@s = REPEAT('word ', N)`:

| N (words) | Before (cubic) | After (quadratic) |
|---:|---:|---:|
| 80   | 0.19 s  | 0.001 s |
| 160  | 1.46 s  | 0.0035 s |
| 320  | 11.6 s  | 0.013 s |
| 640  | *(extrapolated: ~93 s)* | 0.052 s |
| 1280 | *(extrapolated: ~12 min)* | 0.20 s |
| 2560 | *(extrapolated: ~1.6 h)* | 0.80 s |

Scaling per doubling: before ≈ 8× (cubic); after ≈ 4× (quadratic) — confirms
the intended complexity class change, not just a constant-factor speedup.

`SELECT TRIGRAM_STRICT_WORD_SIMILARITY('word', @s)`:

| N (words) | Before | After |
|---:|---:|---:|
| 160  | 0.058 s | 0.0007 s |
| 320  | 0.45 s  | 0.0023 s |
| 640  | 3.7 s   | 0.0087 s |
| 1280 | *(extrapolated: ~30 s)* | 0.034 s |

### Correctness verification

1. The plugin's own MTR suite (`mysql-test/trigram/basic.test`), including
   the `TRIGRAM_STRICT_WORD_SIMILARITY('data', 'metadata database') = 0.4`
   case, still passes byte-for-byte against the recorded `basic.result`.
2. Independently cross-checked against a from-scratch Python re-implementation
   of the **original** brute-force algorithm (not the optimized one), on
   inputs not covered by the existing test suite:

   | Query | Server (after fix) | Python brute-force reference |
   |---|---:|---:|
   | `TRIGRAM_WORD_SIMILARITY('quick fox', 'the quick brown fox jumps over')` | 0.625000 | 0.625 |
   | `TRIGRAM_STRICT_WORD_SIMILARITY('quick fox', 'the quick brown fox jumps over')` | 0.625000 | 0.625 |
   | `TRIGRAM_WORD_SIMILARITY('xyzzy', 'the quick brown fox jumps over the lazy dog repeatedly')` | 0.166667 | 0.166667 |

   Exact match in all cases, confirming the incremental algorithm produces
   identical results to the original, unoptimized one.
