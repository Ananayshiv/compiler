#!/usr/bin/env python3
"""Real-codebase evaluation of CodeScout: ranking quality AND latency.

Task ("find the definition"): for an identifier such as `ReadFull` or
`parse_header`, the query is its natural-language components ("read full",
"parse header") and the relevant lines are every line that DEFINES that exact
identifier. Gold labels come from simple per-language regexes over the source
files, independent of CodeScout's own declaration heuristic.

Reports (per ranking config): MRR@10, Hit@1, Hit@10, paired bootstrap 95% CI
vs plain BM25, plus engine/CLI latency and ripgrep wall-clock on the same tree.

Stdlib only. Example:
    python3 bench/real_world.py --corpus /usr/local/go/src --lang go --n 500 \
        --binary build/codescout --out bench/results_go.json
"""
from __future__ import annotations
import argparse
import itertools
import json
import os
import random
import re
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

SUPPORTED = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".rs", ".py", ".js", ".jsx", ".ts",
             ".tsx", ".java", ".go", ".rb", ".cs", ".swift", ".kt", ".sql", ".md", ".txt", ".toml",
             ".json", ".yaml", ".yml"}
EXCLUDED_DIRS = {".git", ".hg", ".svn", "node_modules", "target", "build", "dist", ".venv", "venv",
                 "__pycache__", ".next", ".idea", ".vscode", "vendor"}
DEF_PATTERNS = {
    "python": [re.compile(r"^\s*(?:async\s+)?def\s+([A-Za-z_]\w*)\s*\("),
               re.compile(r"^\s*class\s+([A-Za-z_]\w*)\s*[(:]")],
    "go": [re.compile(r"^func\s+(?:\([^)]*\)\s*)?([A-Za-z_]\w*)\s*[\[(]"),
           re.compile(r"^type\s+([A-Za-z_]\w*)\s")],
}
LANG_EXT = {"python": ".py", "go": ".go"}


def components(name: str) -> list[str]:
    """Exact mirror of CodeScout's split_identifier() so every query is matchable.

    Splits on '_', lower/digit->Upper and acronym->Word boundaries (HTTPServer ->
    http, server); digit->letter boundaries are NOT split (rsh16x16 stays whole),
    which is a documented tokenizer limitation.
    """
    parts, seg = [], ""
    for i, c in enumerate(name):
        if c == "_":
            if seg: parts.append(seg)
            seg = ""
            continue
        if i > 0 and name[i - 1] != "_" and seg:
            prev = name[i - 1]
            boundary = (c.isupper() and (prev.islower() or prev.isdigit())) or (
                c.isupper() and prev.isupper() and i + 1 < len(name) and name[i + 1].islower())
            if boundary:
                parts.append(seg)
                seg = ""
        seg += c.lower()
    if seg:
        parts.append(seg)
    return parts


def walk(root: Path):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in EXCLUDED_DIRS)
        for f in sorted(filenames):
            p = Path(dirpath, f)
            if p.suffix.lower() in SUPPORTED and not p.is_symlink():
                try:
                    if p.stat().st_size <= 1024 * 1024:
                        yield p
                except OSError:
                    pass


def build_gold(root: Path, lang: str) -> dict[str, set[tuple[str, int]]]:
    gold: dict[str, set[tuple[str, int]]] = {}
    for path in walk(root):
        if path.suffix != LANG_EXT[lang]:
            continue
        rel = path.relative_to(root).as_posix()
        try:
            lines = path.read_bytes().decode("utf-8", "replace").splitlines()
        except OSError:
            continue
        for no, line in enumerate(lines, 1):
            for pat in DEF_PATTERNS[lang]:
                m = pat.match(line)
                if m:
                    gold.setdefault(m.group(1), set()).add((rel, no))
                    break
    return gold


def sample_queries(gold, n: int, seed: int) -> list[dict]:
    eligible = []
    for name, locs in gold.items():
        parts = components(name)
        if len(parts) < 2 or len(set(parts)) < 2 or name.startswith("__") or name.lower().startswith(("test", "benchmark", "example", "fuzz")):
            continue
        if len(locs) > 5:  # very common names (String, Error) are ambiguous, not a "find the definition" task
            continue
        eligible.append({"name": name, "query": " ".join(parts), "gold": sorted(locs)})
    eligible.sort(key=lambda q: q["name"])
    random.Random(seed).shuffle(eligible)
    return eligible[:n]


class Server:
    def __init__(self, binary, index, extra_args):
        self.proc = subprocess.Popen([binary, "serve", "-i", index, *extra_args], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
        self.header = json.loads(self.proc.stdout.readline())

    def query(self, q: str, limit=10) -> dict:
        self.proc.stdin.write(f"{limit}\tall\t\t{q}\n")
        self.proc.stdin.flush()
        return json.loads(self.proc.stdout.readline())

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=30)


def evaluate(server: Server, queries) -> tuple[list[dict], list[float]]:
    rows, latencies = [], []
    for q in queries:
        out = server.query(q["query"])
        latencies.append(out["took_ms"])
        gold = {tuple(g) for g in q["gold"]}
        ranked = [(r["path"], r["line"]) for r in out["results"]]
        rank = next((i for i, loc in enumerate(ranked, 1) if loc in gold), None)
        rows.append({"rr": 1 / rank if rank else 0.0, "hit1": float(rank == 1),
                     "hit10": float(rank is not None)})
    return rows, latencies


def paired_ci(a, b, iterations=2000, seed=0):
    delta = [y - x for x, y in zip(a, b)]
    rng = random.Random(seed)
    boots = sorted(statistics.fmean(rng.choices(delta, k=len(delta))) for _ in range(iterations))
    return boots[int(0.025 * (iterations - 1))], boots[int(0.975 * (iterations - 1))]


def pct(values, p):
    values = sorted(values)
    return values[min(len(values) - 1, int(round(p / 100 * (len(values) - 1))))]


def ripgrep_pattern(parts):
    # Same AND-of-terms-on-one-line semantics (case-insensitive substrings, any order).
    alts = ["".join(f"(?:{re.escape(p)}).*" for p in perm)[:-2] for perm in itertools.permutations(parts)]
    return "|".join(alts) if len(alts) <= 24 else ".*".join(map(re.escape, parts))


def time_ripgrep(root, queries, ext) -> list[float]:
    times = []
    for q in queries:
        start = time.perf_counter()
        subprocess.run(["rg", "-n", "-i", "--no-heading", "-g", f"*{ext}", "-e",
                        ripgrep_pattern(q["query"].split()), str(root)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        times.append((time.perf_counter() - start) * 1000)
    return times


def time_cli(binary, index, queries) -> list[float]:
    times = []
    for q in queries:
        start = time.perf_counter()
        subprocess.run([binary, "search", q["query"], "-i", index, "--json"], stdout=subprocess.DEVNULL)
        times.append((time.perf_counter() - start) * 1000)
    return times


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--lang", choices=sorted(DEF_PATTERNS), required=True)
    ap.add_argument("--binary", default="build/codescout")
    ap.add_argument("--n", type=int, default=500)
    ap.add_argument("--seed", type=int, default=13,
                    help="13 = development sample; final held-out numbers use a fresh seed")
    ap.add_argument("--grid", action="store_true", help="sweep boosts (use on the TUNING corpus only)")
    ap.add_argument("--latency-sample", type=int, default=50)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    root = Path(args.corpus)

    with tempfile.TemporaryDirectory() as tmp:
        index = str(Path(tmp, "corpus.csix"))
        start = time.perf_counter()
        subprocess.run([args.binary, "index", str(root), "-o", index], check=True, stdout=subprocess.DEVNULL)
        build_s = time.perf_counter() - start
        index_mb = Path(index).stat().st_size / 2**20

        gold = build_gold(root, args.lang)
        queries = sample_queries(gold, args.n, args.seed)

        configs = {"bm25": ["--rank", "bm25"], "code_aware(default)": ["--rank", "code"]}
        if args.grid:
            for d, p in itertools.product([0.25, 0.5, 1.0, 2.0, 4.0], [0.0, 0.25, 0.5, 1.0]):
                configs[f"code d={d} p={p}"] = ["--rank", "code", "--def-boost", str(d), "--path-boost", str(p)]

        results, latencies, header = {}, {}, None
        for name, extra in configs.items():
            start = time.perf_counter()
            server = Server(args.binary, index, extra)
            load_s = time.perf_counter() - start
            header = server.header
            rows, lat = evaluate(server, queries)
            server.close()
            results[name] = rows
            latencies[name] = lat
            latencies.setdefault("_load_s", load_s)

        base = results["bm25"]
        summary = {}
        for name, rows in results.items():
            entry = {k: statistics.fmean(r[k] for r in rows) for k in ("rr", "hit1", "hit10")}
            if name != "bm25":
                lo, hi = paired_ci([r["rr"] for r in base], [r["rr"] for r in rows])
                entry["delta_mrr_vs_bm25"] = entry["rr"] - statistics.fmean(r["rr"] for r in base)
                entry["delta_mrr_ci95"] = [lo, hi]
            entry["engine_ms_p50"] = pct(latencies[name], 50)
            entry["engine_ms_p95"] = pct(latencies[name], 95)
            summary[name] = entry

        sample = queries[: args.latency_sample]
        rg_ms = time_ripgrep(root, sample, LANG_EXT[args.lang])
        cli_ms = time_cli(args.binary, index, sample)

    report = {
        "corpus": str(root), "lang": args.lang, "seed": args.seed, "n_queries": len(queries),
        "eligible_definitions": sum(1 for _ in gold), "index": {
            "lines": header["lines"], "terms": header["terms"], "declaration_lines": header["definitions"],
            "build_s": build_s, "snapshot_mb": index_mb, "load_s": latencies["_load_s"]},
        "ranking": summary,
        "latency_ms": {
            "codescout_warm_engine_p50": summary["code_aware(default)"]["engine_ms_p50"],
            "codescout_warm_engine_p95": summary["code_aware(default)"]["engine_ms_p95"],
            "codescout_cli_cold_p50": pct(cli_ms, 50), "codescout_cli_cold_p95": pct(cli_ms, 95),
            "ripgrep_p50": pct(rg_ms, 50), "ripgrep_p95": pct(rg_ms, 95), "latency_queries": len(sample)},
        "example_queries": queries[:5],
    }
    Path(args.out).write_text(json.dumps(report, indent=2))
    best = sorted(summary.items(), key=lambda kv: -kv[1]["rr"])[:8]
    print(json.dumps({"index": report["index"], "latency_ms": report["latency_ms"],
                      "top_configs": {k: {m: round(v, 4) if isinstance(v, float) else v for m, v in e.items()}
                                      for k, e in best},
                      "bm25": summary["bm25"]}, indent=1))


if __name__ == "__main__":
    main()
