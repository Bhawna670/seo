# Advanced Search Engine: Ranking & Autocomplete

![CI](https://github.com/YOUR_USERNAME/advanced-search-engine/actions/workflows/ci.yml/badge.svg)

A command-line search engine in C++ that indexes plain-text documents and supports ranked
search (TF-IDF and BM25), autocomplete, spelling correction, Boolean and phrase queries,
field-based search, and persistent search history. No third-party libraries.

<!-- Add 2-3 screenshots here, for example:
![Ranked search](docs/ranked-search.png)
![TF-IDF vs BM25](docs/compare.png)
-->

## Highlights

- **Ranking:** TF-IDF with cosine similarity and Okapi BM25, searchable by title, content or both
- **Autocomplete:** a Trie of the vocabulary merged with your own past searches, ranked by frequency
- **Autocorrect:** Levenshtein-distance correction with a "Did you mean" notice
- **Queries:** Boolean `AND` / `OR` / `NOT`, exact `"phrase"` search, highlighted matches
- **Persistence:** search history is saved to disk and restored on the next start
- **Engineering:** modular code base, result cache, 52 automated tests, built-in benchmark and diagnostics, CI on Linux and Windows

## Quick start

```
git clone https://github.com/YOUR_USERNAME/advanced-search-engine.git
cd advanced-search-engine
make                 # Windows (MinGW): build.bat
./search_engine      # Windows: search_engine.exe
```

Run it from the project root so it finds the `Data/` folder. Sample documents are included;
add any `.txt` files to `Data/` to search your own.

Run the tests:

```
make test            # or: ./search_engine --selftest
```

Options:

```
search_engine [dataFolder] [historyFile]
search_engine --selftest
```

## Menu

```
 1. Search Documents (TF-IDF)          7. Compare TF-IDF vs BM25
 2. Display Inverted Index             8. Query Suggestions (from past searches)
 3. Autocomplete (vocab + history)     9. Clear Search History
 4. Advanced Search (Boolean/phrase)  10. System Diagnostics
 5. Search History                    11. Performance Benchmark
 6. BM25 Search (Title/Content/Both)  12. Exit
```

| Try this query | Menu | What happens |
|----------------|------|--------------|
| `bm25 ranking` | 1 or 6 | Ranked results with highlighted terms |
| `serch enginee` | 1 or 6 | Autocorrected to `search engine` |
| `"inverted index"` | 4 | Exact phrase match |
| `index AND trie` | 4 | Boolean AND |
| `rank` | 3 | Completions from vocabulary and past searches |

## Project structure

```
src/
├── main.cpp               Menu and program entry point
├── common.h               Shared types and constants
├── text_utils.h/.cpp      Tokenization, normalization, highlighting
├── corpus.h/.cpp          Document storage and loading from disk
├── index.h/.cpp           Inverted index, TF-IDF vectors, BM25 statistics
├── ranking.h/.cpp         TF-IDF and BM25 ranking, result cache
├── autocorrect.h/.cpp     Levenshtein spelling correction
├── history.h/.cpp         Persistent history and query suggestions
├── autocomplete.h/.cpp    Trie + history autocomplete
├── structured_search.h/.cpp  Boolean and phrase matching
├── search.h/.cpp          User-facing search commands
├── diagnostics.h/.cpp     Statistics, health checks, benchmark
├── selftest.h/.cpp        Automated tests
└── Trie.h/.cpp            Trie data structure
Data/                      Sample documents
```

Dependencies flow one way: `text_utils` and `common` at the bottom, then `corpus` and `index`,
then `ranking`, `autocorrect`, `history`, `autocomplete` and `structured_search`, and
`search`, `diagnostics`, `selftest` and `main` on top.

## How it works

**Preprocessing.** Text is lowercased, non-alphanumeric characters become separators and
stop words are dropped. Each document is tokenized once at startup.

**Inverted index.** `term -> {docId: termFrequency}` with document and corpus frequencies.
Searches only score documents that contain at least one query term.

**TF-IDF.** `weight = (tf / docLength) * (ln((N + 1) / (df + 1)) + 1)`, compared with the
query by cosine similarity.

**BM25.** `idf * tf * (k1 + 1) / (tf + k1 * (1 - b + b * len / avgLen))` with k1 = 1.5,
b = 0.75 and `idf = ln(1 + (N - df + 0.5) / (df + 0.5))`. Title and content have separate
indexes and length statistics; in "both" mode title matches count double.

**Autocorrect.** Words missing from the vocabulary are replaced by the closest vocabulary
word (edit distance 1 for 3-5 letter words, 2 for longer ones; ties go to the more frequent
word). Words containing digits are left alone.

**Autocomplete.** Past queries starting with the typed text come first, then Trie
completions of the last word ranked by corpus frequency, without duplicates.

**Persistence.** Each query is appended to `search_history.txt` immediately. On startup the
file is loaded and query frequencies are rebuilt.

**Performance work.** Documents are tokenized and phrase-normalized once, top-K uses
`partial_sort`, the query norm is computed once per query, and an LRU-style result cache
answers repeated queries (term order does not matter).

## Testing and performance

`--selftest` runs 52 checks on a built-in four-document corpus, independent of `Data/`:
tokenization, index statistics, TF-IDF and BM25 ranking, field search, Levenshtein distance,
autocorrect, Boolean operators, phrase boundaries, highlighting, autocomplete, history
save/reload/clear and the cache. It exits with code 0 on success, and GitHub Actions runs it
on Linux and Windows for every push. The code is also clean under AddressSanitizer and
UBSan.

Measured on a synthetic corpus of 2,000 documents (about 612,000 tokens, 5,976 unique terms),
built with `g++ -O2` on Linux:

| Operation | Result |
|-----------|--------|
| Index build | about 490 ms |
| TF-IDF query (top 5) | about 63 us |
| BM25 query (top 5) | about 26 us |
| Autocorrect one word | about 270 us |

Run menu option 11 to measure on your own machine; results vary with hardware.

## Known limitations

- Boolean queries are flat: evaluated left to right, no precedence or parentheses.
- The Trie stores letters a-z only, so terms with digits are searchable but not offered by
  Trie autocomplete.
- Field selection applies to BM25; TF-IDF and the comparison use the content field.
- Stop words are removed, so a Boolean query on a word like `the` returns nothing.
- Only `.txt` files are indexed and the index is rebuilt at every start.
- State is held in module-level globals, which keeps the code simple but would be wrapped in
  an `Engine` class for a larger project.

## Future work

Parentheses in Boolean queries, stemming, a positional index for faster phrase search, saving
the index to disk, and an HTTP interface.

## License

MIT, see [LICENSE](LICENSE).