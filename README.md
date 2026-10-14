# CodeScout — Code-Aware Search Engine in C++17

**Search large codebases without scanning every file on every query.** CodeScout is a dependency-free command-line search engine that indexes source files using an inverted index and ranks matching lines with **BM25**. Its tokenizer understands `camelCase`, `HTTPAcronyms`, and `snake_case`, so `http header` can match `parseHTTPHeader`.

> Built as an approachable core CS project: inverted indexes (data structures), ranking (information retrieval), file traversal and persistence (systems), and reproducible performance measurement.

## Features

- Source-code-aware tokenization: whole identifiers + camelCase/acronym/snake_case components
- Inverted index with per-document term frequencies and BM25 ranking
- AND queries (default), OR queries (`--or`), file-path filters and configurable result limits
- Line-level results with original filenames, line numbers and snippets
- Portable on-disk snapshot (`.csix`), with deterministic scan order and basic corruption checks
- Excludes build artifacts, dependency directories, large files and common binary data
- CMake build, unit tests, GitHub Actions CI, and a reproducible local benchmark
- **No external C++ dependencies**

## Quick start

Requirements: **C++17 compiler**, **CMake 3.16+**.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# Search the sample project
./build/codescout index examples -o demo.csix
./build/codescout search "http header" -i demo.csix

# Index and search your own repository
./build/codescout index /path/to/your/repo -o myrepo.csix
./build/codescout search "parse token" -i myrepo.csix --limit 5
./build/codescout search "cache parser" -i myrepo.csix --or
./build/codescout search "request" -i myrepo.csix --path .cpp
./build/codescout stats -i myrepo.csix
./build/codescout bench
```

Windows: execute `build\\Debug\\codescout.exe` for multi-config CMake generators or `build\\codescout.exe` for single-config builds.

## How it works

```text
Source files (.cpp, .rs, .py, .ts, ...)
                 │
                 ▼
   Code-aware lexer / tokenizer
     ├── parseHTTPHeader
     └── parse | http | header
                 │
                 ▼
     Inverted index (term → postings)
                 │
                 ▼
      BM25 scoring of candidates
                 │
                 ▼
    Sorted file:line + code snippet
```

Each nonempty source line becomes one document. A posting stores `(line_id, term_frequency)`. A query visits only posting lists for its terms, scores candidates with BM25, then selects the best matches. Source lines are stored in the `.csix` file, so search does not reread the original source files. Loading the snapshot rebuilds the in-memory inverted index.

BM25 (for each matching term):

```text
score = IDF × tf × (k1 + 1) / [tf + k1 × (1 - b + b × doc_len / avg_len)]
k1 = 1.2, b = 0.75
IDF = ln[1 + (N - df + 0.5) / (df + 0.5)]
```

**Complexity:** indexing is proportional to input text size on average; queries primarily visit term postings and sort the candidate results. This is much cheaper than rescanning every source file for each query when terms are selective.

## Benchmark

Run `./build/codescout bench` for an automatically generated **20,000-line** synthetic corpus and **48 search operations**. It prints the measured latency for the indexed top-10 BM25 path and brute-force top-10 BM25 search, using identical tokenization and ranking and asserting that their result checksums match.

**Important:** Both implementations have the same query semantics; the benchmark measures query execution only. However, the synthetic corpus is favorable to the inverted index, and loading/index construction is excluded. Do not generalize the speed ratio to real codebases without measuring. Hardware, compiler flags, corpus, and query distribution matter.

For a fairer evaluation, compare identical output semantics on your target repository, separately report indexing and snapshot-load time, and benchmark p50/p95 latency after warmup.

## Tests

`tests/test_search.cpp` validates identifier tokenization, AND/OR queries, ranking, filtering, snapshot roundtrip, repository traversal and malformed snapshots. CI builds and runs the tests on Ubuntu; query examples are also smoke-tested.

## Limitations / honest design choices

- Line-level index, not full-code semantics; queries may miss relationships across lines.
- Whole-identifier and component tokens are indexed, which trades additional memory for flexible search.
- The entire postings structure resides in RAM, and loading a snapshot rebuilds postings.
- Snapshot format is versioned (`CSIX01`) but intended as a local trusted artifact, not a hostile-input format.
- ASCII-oriented tokenization; Unicode-aware identifiers are not supported.
- Batch index building only; no incremental updates, Git history search or regular expressions.
- Ranking returns relevance scores, not exact source-code execution semantics.

## What to discuss in interviews

1. Why an inverted index reduces query work relative to line-by-line scanning.
2. What the posting list stores and why it has both document IDs and term frequency.
3. How BM25 differs from a raw frequency count and why document length normalization matters.
4. Why camelCase identifiers require special tokenization.
5. Why indexes consume additional space and how to implement incremental updates.
6. Which metrics would establish a fair performance comparison.

## Folder structure

```text
include/codescout.hpp       Public search/index interface
src/codescout.cpp           Tokenizer, indexing, BM25, snapshot persistence
src/main.cpp                CLI and optional synthetic benchmark
tests/test_search.cpp       Assertions / unit tests
examples/sample.cpp        Example search corpus
.github/workflows/ci.yml   Linux build/test workflow
```

MIT licensed.
