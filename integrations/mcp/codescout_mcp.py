#!/usr/bin/env python3
"""CodeScout as an MCP (Model Context Protocol) tool server for coding agents.

Stdlib only. Speaks JSON-RPC 2.0 over stdio (newline-delimited) and keeps one
`codescout serve` process alive, so every agent query hits a warm in-memory
index (~0.2-2 ms engine time) instead of reloading the snapshot.

    codescout index /path/to/repo -o repo.csix
    python3 integrations/mcp/codescout_mcp.py --index repo.csix --binary build/codescout

Claude Code:  claude mcp add codescout -- python3 /abs/path/codescout_mcp.py --index /abs/repo.csix --binary /abs/codescout
Tools exposed: `search_code`, `index_stats`.
"""
from __future__ import annotations
import argparse
import json
import subprocess
import sys
import threading

PROTOCOL_VERSION = "2025-06-18"
TOOLS = [
    {
        "name": "search_code",
        "description": (
            "Ranked code search over the indexed repository. Splits camelCase/snake_case, ranks "
            "with BM25 and boosts lines that DEFINE a matching symbol, so 'read full' finds "
            "`func ReadFull(...)` before its call sites. Returns file:line, the line text, and "
            "whether it is a definition. Use plain words, not regex."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Words or identifiers, e.g. 'parse http header'"},
                "limit": {"type": "integer", "minimum": 1, "maximum": 50, "default": 10},
                "match": {"type": "string", "enum": ["all", "any"], "default": "all",
                          "description": "all = every term on the line; any = at least one"},
                "path": {"type": "string", "description": "Only files whose path contains this substring"},
            },
            "required": ["query"],
        },
    },
    {
        "name": "index_stats",
        "description": "Size of the loaded CodeScout index (lines, unique terms, declaration lines).",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


class Engine:
    def __init__(self, binary: str, index: str):
        self.proc = subprocess.Popen([binary, "serve", "-i", index], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True, encoding="utf-8",
                                     errors="replace")
        header = self.proc.stdout.readline()
        if not header:
            raise RuntimeError(f"codescout serve failed to start for index {index!r}")
        self.stats = json.loads(header)
        self.lock = threading.Lock()

    def search(self, query: str, limit: int, match: str, path: str) -> dict:
        for field in (query, path):
            if "\t" in field or "\n" in field or "\r" in field:
                raise ValueError("query/path must not contain tabs or newlines")
        with self.lock:
            self.proc.stdin.write(f"{limit}\t{match}\t{path}\t{query}\n")
            self.proc.stdin.flush()
            return json.loads(self.proc.stdout.readline())


def format_results(payload: dict) -> str:
    results = payload.get("results", [])
    if not results:
        return f"No lines match {payload.get('query')!r}. Try fewer words or match='any'."
    lines = [f"{len(results)} results for {payload['query']!r} ({payload['took_ms']:.2f} ms):"]
    for r in results:
        tag = f" [defines {r['symbol']}]" if r.get("definition") else ""
        lines.append(f"{r['path']}:{r['line']}{tag}  {r['text'].strip()}")
    return "\n".join(lines)


def handle(engine: Engine, message: dict) -> dict | None:
    method, mid = message.get("method"), message.get("id")
    if mid is None:                       # notification (e.g. notifications/initialized)
        return None
    try:
        if method == "initialize":
            result = {"protocolVersion": PROTOCOL_VERSION, "capabilities": {"tools": {}},
                      "serverInfo": {"name": "codescout", "version": "2.0.0"}}
        elif method == "ping":
            result = {}
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            params = message.get("params", {})
            name, args = params.get("name"), params.get("arguments") or {}
            if name == "index_stats":
                text, structured = json.dumps(engine.stats), engine.stats
            elif name == "search_code":
                query = str(args.get("query", "")).strip()
                if not query:
                    raise ValueError("query is required")
                limit = max(1, min(50, int(args.get("limit", 10))))
                match = args.get("match", "all")
                if match not in ("all", "any"):
                    raise ValueError("match must be 'all' or 'any'")
                payload = engine.search(query, limit, match, str(args.get("path", "")))
                if "error" in payload:
                    raise ValueError(payload["error"])
                text, structured = format_results(payload), payload
            else:
                return {"jsonrpc": "2.0", "id": mid,
                        "error": {"code": -32602, "message": f"Unknown tool: {name}"}}
            result = {"content": [{"type": "text", "text": text}], "structuredContent": structured,
                      "isError": False}
        else:
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"Method not found: {method}"}}
    except (ValueError, TypeError) as exc:  # tool-level errors are reported to the model, not as protocol errors
        result = {"content": [{"type": "text", "text": f"Error: {exc}"}], "isError": True}
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--index", required=True)
    ap.add_argument("--binary", default="codescout")
    args = ap.parse_args()
    engine = Engine(args.binary, args.index)
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            message = json.loads(raw)
        except json.JSONDecodeError:
            response = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}}
        else:
            response = handle(engine, message)
        if response is not None:
            sys.stdout.write(json.dumps(response, ensure_ascii=False) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
