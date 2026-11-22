# CodeScout — Code-Aware Search Engine in C++17

**Find where things are defined, not just where words appear.** CodeScout is a dependency-free C++17 search engine for source code: an inverted index with BM25, a tokenizer that understands `camelCase` / `snake_case` / `HTTPAcronyms`, and a **definition-aware ranker** that puts the line *declaring* a symbol above its call sites. It runs as a CLI, a persistent JSON-lines server, and an **MCP tool server for coding agents**.

## Results on real codebases

Task: given the words of an identifier (`read full`), rank the line that defines it (`func ReadFull(...)`) first. Defaults tuned on the Python stdlib, then evaluated on the **held-out Go standard library** (fresh sample, different language). Full protocol: [`bench/RESULTS.md`](bench/RESULTS.md).

| Held-out: Go 1.24 stdlib, 1.52M lines, 500 queries | MRR@10 | Hit@1 | Hit@10 |
|---|---:|---:|---:|
| BM25 baseline | 0.379 | 16.8% | 78.6% |
| **CodeScout code-aware ranking** | **0.946** | **91.0%** | **99.4%** |

ΔMRR **+0.567** (paired bootstrap 95% CI [+0.539, +0.596]). Warm queries: **2.4 ms p50** on 1.5M lines vs **62 ms** for ripgrep over the same files.

## Features

- **Definition-aware ranking**: a language-agnostic declaration detector (Python, Go, Rust, C/C++, Java, JS/TS, `#define`) records the symbol each line declares; matching definitions get a boost scaled by how *precisely* the name matches the query (Jaccard over name components), so `specialsIter` beats `newSpecialsIter` for "specials iter"
- Source-code-aware tokenization: whole identifiers + camelCase/acronym/snake_case components
- Inverted index with per-line term frequencies and BM25 (`--rank bm25` keeps the plain baseline)
- **CSIX02 snapshot** persists the full inverted index (postings, lengths, symbols): 3× faster load than re-tokenizing, atomic writes, deterministic bytes, bounds-checked parsing; legacy CSIX01 files still load
- **`serve` mode**: keeps the index in memory and answers JSON-lines requests (sub-millisecond to a few ms per query)
- **MCP server** ([`integrations/mcp/`](integrations/mcp/)) exposing `search_code` / `index_stats` to Claude Code, Cursor and other MCP clients
- `--json` output, AND/OR queries, path filters, result limits
- Unit tests (tokenizer, declaration detector across 7 languages, ranking, snapshots, corruption), MCP end-to-end test, ASan/UBSan CI job
- **No external C++ dependencies**

## Quick start

Requirements: **C++17 compiler**, **CMake 3.16+** (Python 3 only for the benchmark and MCP server).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/codescout index /path/to/your/repo -o repo.csix
./build/codescout search "parse header" -i repo.csix --limit 5      # (def) marks definitions
./build/codescout search "cache parser" -i repo.csix --or --json
./build/codescout search "request" -i repo.csix --path .cpp --rank bm25
./build/codescout stats -i repo.csix
printf 'read full\n5\tany\tnet/\thttp header\n' | ./build/codescout serve -i repo.csix
```

`serve` protocol: one request per line, either a bare query or `limit<TAB>all|any<TAB>path-filter<TAB>query`; one JSON object per response line.

### Use it from a coding agent (MCP)

```bash
./build/codescout index ~/code/myrepo -o ~/myrepo.csix
claude mcp add codescout -- python3 $PWD/integrations/mcp/codescout_mcp.py \
    --index ~/myrepo.csix --binary $PWD/build/codescout
```

The agent gets a `search_code` tool whose results mark which lines define which symbols, and every call hits the warm in-memory index.

### Reproduce the evaluation

```bash
python3 bench/real_world.py --corpus /usr/lib/python3.12 --lang python --grid --out bench/results_python_tuning.json
python3 bench/real_world.py --corpus "$(go env GOROOT)/src" --lang go --seed 29 --out bench/results_go_heldout.json
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

Each nonempty source line becomes one document. A posting stores `(line_id, term_frequency)`. A query visits only posting lists for its terms, scores candidates with BM25, then selects the best matches. Source lines, postings and declaration symbols are stored in the `.csix` snapshot, so search neither rereads source files nor re-tokenizes them on load.

BM25 (for each matching term):

```text
score = IDF × tf × (k1 + 1) / [tf + k1 × (1 - b + b × doc_len / avg_len)]
k1 = 1.2, b = 0.75
IDF = ln[1 + (N - df + 0.5) / (df + 0.5)]
```

Code-aware ranking (default) multiplies BM25 by a definition boost:

```text
final = BM25 × (1 + 2.0 × name_match + 0.0 × path_match)
name_match = 1 if the query contains the whole identifier, else
             |query terms ∩ name components| / |query terms ∪ name components|   (only for declaration lines)
```

Weights were tuned on the Python stdlib and frozen before the Go held-out evaluation; the path term is implemented but off because it did not help (see `bench/RESULTS.md`).

**Complexity:** indexing is proportional to input text size on average; queries primarily visit term postings and sort the candidate results. This is much cheaper than rescanning every source file for each query when terms are selective.

## Benchmark

See [`bench/RESULTS.md`](bench/RESULTS.md) for ranking quality and latency on the Python and Go standard libraries, including where ripgrep is faster (one-shot cold queries) and why. `./build/codescout bench` remains as a synthetic indexed-vs-brute-force cross-check with identical, checksummed results; its speed ratio favours the index by construction and is not a headline claim.

## Tests

`tests/test_search.cpp` validates identifier tokenization, the declaration detector (positive and negative cases across languages), BM25 vs code-aware ranking, path boosts, JSON escaping, CSIX02 round-trips with identical scores, deterministic snapshot bytes, truncated/tampered/trailing-data rejection, legacy CSIX01 loading, AND/OR queries, filtering and repository traversal. `integrations/mcp/test_mcp.py` drives the MCP server end to end. CI runs both on Ubuntu plus an AddressSanitizer/UBSan build.

## Limitations / honest design choices

- Line-level index, not full-code semantics; queries may miss relationships across lines, and declarations split across lines are not detected.
- The declaration detector is a heuristic, not a parser; unusual syntax can be missed or misread.
- Digit–letter boundaries are not split (`rsh16x16` is one token).
- One-shot CLI searches must load the snapshot first (≈0.2 s for 240K lines, ≈1.2 s for 1.5M); ripgrep is faster for single ad-hoc searches. Use `serve`/MCP for repeated queries.
- Lexical, not semantic: "remove item" will not find `deleteEntry`. Embedding-based hybrid retrieval is the natural next step.
- Whole-identifier and component tokens are indexed, which trades additional memory for flexible search.
- The entire postings structure resides in RAM (the Go stdlib index is a 194 MB snapshot).
- Snapshot format is versioned (`CSIX02`, reads `CSIX01`) and bounds-checked, but intended as a local trusted artifact, not a hostile-input format.
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
7. Why plain BM25 ranks call sites above definitions, and how the held-out protocol avoids tuning on the test set.
8. Why a warm server beats ripgrep while a one-shot CLI call does not.

## Folder structure

```text
include/codescout.hpp       Public search/index interface
src/codescout.cpp           Tokenizer, indexing, BM25, snapshot persistence
src/main.cpp                CLI, serve mode and synthetic benchmark
tests/test_search.cpp       Assertions / unit tests
bench/real_world.py         Real-codebase ranking + latency evaluation
bench/RESULTS.md            Measured results and protocol
integrations/mcp/           MCP tool server for coding agents + e2e test
examples/sample.cpp        Example search corpus
.github/workflows/ci.yml   Linux build/test workflow
```

MIT licensed.
