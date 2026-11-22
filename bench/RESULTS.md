# CodeScout — real-codebase results

All numbers below were measured by `bench/real_world.py` on a 2-vCPU Intel Xeon
@ 2.10 GHz (Linux, GCC 13, `-DCMAKE_BUILD_TYPE=Release`). Raw outputs are in the
JSON files next to this document. Re-run them to reproduce; absolute latencies
vary with hardware.

## Task: "find the definition"

A developer remembers what something does but not its exact name. For an
identifier such as `ReadFull` or `parse_header`, the **query is its words**
(`read full`, `parse header`) and the **relevant lines are every line that
defines that exact identifier**. Gold labels come from simple per-language
regexes (`def`/`class` for Python; `func`/`type` for Go), written
independently of CodeScout's own declaration detector.

Sampling: identifiers with ≥ 2 distinct name components, excluding
test/benchmark/example/fuzz helpers, dunder names, and names defined in more
than 5 places (e.g. `String`, `Error`), which are ambiguous rather than
navigational. 500 queries per corpus, AND matching, top-10.

## Protocol (no test-set tuning)

1. **Tune on Python 3.12 stdlib** (seed 13): sweep `definition_boost` ∈
   {0.25, 0.5, 1, 2, 4} × `path_boost` ∈ {0, 0.25, 0.5, 1}
   (`results_python_tuning.json`). MRR plateaus for `definition_boost ≥ 2`;
   the path boost never helped, so it ships **off** (default 0).
2. Defaults frozen at `definition_boost = 2.0, path_boost = 0.0`.
3. **Test on the Go 1.24 standard library** — a different language and a
   6× larger tree — with a **fresh sample (seed 29)** never inspected during
   development (`results_go_heldout.json`). An earlier Go sample (seed 13) *was*
   used for error analysis, which led to the name-precision (Jaccard) match;
   that is why the final held-out sample uses a new seed.

## Ranking quality

| Corpus | Lines indexed | Ranking | MRR@10 | Hit@1 | Hit@10 |
|---|---:|---|---:|---:|---:|
| Python 3.12 stdlib (tuning) | 239,132 | BM25 | 0.546 | 35.0% | 92.0% |
| | | **Code-aware** | **0.960** | **93.4%** | **100%** |
| Go 1.24 stdlib (**held-out**) | 1,521,052 | BM25 | 0.379 | 16.8% | 78.6% |
| | | **Code-aware** | **0.946** | **91.0%** | **99.4%** |

Held-out ΔMRR = **+0.567**, paired bootstrap 95% CI **[+0.539, +0.596]**
(2,000 resamples over queries). Tuning-set ΔMRR = +0.414 [+0.383, +0.444].

Why plain BM25 loses: call sites repeat the words (`n, err := ReadFull(r, b)
// read full frame`) and short lines get BM25's length-normalisation bonus, so
usages outrank the one line that defines the symbol.

Remaining failure modes seen in error analysis:
- Two genuine definitions with the same words (`WriteRune` vs. unexported
  `writeRune`); the query cannot disambiguate.
- Digit–letter boundaries are not split by the tokenizer (`rsh16x16` stays one
  token), so `rsh16 x16` does not match.
- Declarations spanning several lines (signature broken after `func`) are not
  detected as definitions.

## Latency (same machine, 50 queries)

| | Python stdlib (239K lines) | Go stdlib (1.52M lines) |
|---|---:|---:|
| Index build (scan + tokenize + save) | 0.97 s | 5.38 s |
| Snapshot size (CSIX02) | 28 MB | 194 MB |
| Snapshot load | 0.18 s | 1.23 s |
| **Warm query, in-memory engine** p50 / p95 | **0.19 / 1.35 ms** | **2.4 / 16.0 ms** |
| One-shot CLI (load + query) p50 | 194 ms | 1,228 ms |
| ripgrep 14 (same files, AND-of-terms regex) p50 / p95 | 11.6 / 29.2 ms | 62 / 138 ms |

Reading this honestly:
- A **warm** index (`codescout serve`, the MCP server) answers ~25–60× faster
  than ripgrep re-scanning the tree, **and** returns ranked results.
- A **one-shot** CLI call is slower than ripgrep because the snapshot must be
  loaded first. CSIX02 cut that load ~3× versus CSIX01 (which re-tokenized every
  line: 596 ms → 194 ms on Python), but for single ad-hoc searches ripgrep is
  the better tool. CodeScout's niche is repeated queries (agents, editors).
- ripgrep matches case-insensitive substrings; CodeScout matches tokens. The
  comparison is about latency on equivalent AND-of-terms intent, not identical
  result sets.

## Synthetic micro-benchmark (`codescout bench`)

Indexed vs. brute-force BM25 with identical semantics (checksummed) on 20K
generated lines: ~35–60× faster queries (noisy on a shared 2-vCPU VM). This
favours the inverted index by construction and is kept only as a correctness
cross-check; use the real-codebase table above for claims.
