#!/usr/bin/env python3
"""End-to-end MCP smoke test: python3 integrations/mcp/test_mcp.py build/codescout"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

binary = sys.argv[1] if len(sys.argv) > 1 else "build/codescout"
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as tmp:
    repo = Path(tmp, "repo")
    repo.mkdir()
    (repo / "io.go").write_text("func ReadFull(r Reader, buf []byte) error {\n"
                                "\treturn nil\n}\n")
    (repo / "use.go").write_text("n, err := ReadFull(r, buf) // read full frame\n")
    index = str(Path(tmp, "r.csix"))
    subprocess.run([binary, "index", str(repo), "-o", index], check=True, stdout=subprocess.DEVNULL)
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
         "params": {"name": "search_code", "arguments": {"query": "read full", "limit": 5}}},
        {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
         "params": {"name": "search_code", "arguments": {"query": "bad\tquery"}}},
        {"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "index_stats"}},
        {"jsonrpc": "2.0", "id": 6, "method": "nope"},
    ]
    out = subprocess.run([sys.executable, str(here / "codescout_mcp.py"), "--index", index, "--binary", binary],
                         input="\n".join(json.dumps(m) for m in msgs) + "\nnot json\n",
                         capture_output=True, text=True, timeout=30, check=True).stdout
    replies = {r["id"]: r for r in map(json.loads, out.splitlines())}
    assert replies[1]["result"]["serverInfo"]["name"] == "codescout"
    assert {t["name"] for t in replies[2]["result"]["tools"]} == {"search_code", "index_stats"}
    top = replies[3]["result"]["structuredContent"]["results"][0]
    assert top["path"] == "io.go" and top["definition"] and top["symbol"] == "ReadFull", top
    assert "[defines ReadFull]" in replies[3]["result"]["content"][0]["text"]
    assert replies[4]["result"]["isError"] is True
    assert replies[5]["result"]["structuredContent"]["definitions"] == 1
    assert replies[6]["error"]["code"] == -32601
    assert replies[None]["error"]["code"] == -32700
    assert len(replies) == 7  # notification got no reply
print("MCP smoke test passed")
