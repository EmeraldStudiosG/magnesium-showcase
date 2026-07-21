#!/usr/bin/env python3
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "magnesium"
URI = "file:///tmp/magnesium_lsp_smoke.mg"

SOURCE = """let input = input("score")
if input > 10 then
    print(input)
end

type PlayerData = &< name: string, score: number >
extern fn host_score(): number

fn add(a, b)
    return a + b
end

let config = &< port = 8080 >
print(dict.get(config, "port"))
print(math.sqrt(4))
print(add(1, 2))
"""

STRICT_BAD_SOURCE = """!strict
let score: number = "bad"
score()
"""


def send(proc, payload):
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    proc.stdin.write(b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n\r\n" + body)
    proc.stdin.flush()


def read_message(proc):
    headers = {}
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("language server exited")
        if line in (b"\r\n", b"\n"):
            break
        key, value = line.decode("ascii").split(":", 1)
        headers[key.lower()] = value.strip()
    length = int(headers["content-length"])
    return json.loads(proc.stdout.read(length).decode("utf-8"))


def request(proc, counter, method, params):
    counter[0] += 1
    req_id = counter[0]
    send(proc, {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params})
    while True:
        msg = read_message(proc)
        if msg.get("id") == req_id:
            return msg


def position_of(needle):
    before = SOURCE.index(needle)
    line = SOURCE.count("\n", 0, before)
    line_start = SOURCE.rfind("\n", 0, before) + 1
    return {"line": line, "character": before - line_start}


def main():
    proc = subprocess.Popen(
        [str(BIN), "--lsp"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    counter = [0]
    try:
        init = request(proc, counter, "initialize", {"processId": None, "rootUri": str(ROOT)})
        caps = init["result"]["capabilities"]
        assert "completionProvider" in caps
        assert caps.get("hoverProvider") is True
        assert caps.get("documentSymbolProvider") is True
        assert caps.get("definitionProvider") is True
        assert caps.get("renameProvider") is True
        assert caps.get("documentFormattingProvider") is True
        assert "semanticTokensProvider" in caps

        send(proc, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
        send(
            proc,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": URI,
                        "languageId": "magnesium",
                        "version": 1,
                        "text": SOURCE,
                    }
                },
            },
        )

        diagnostics = None
        while diagnostics is None:
            msg = read_message(proc)
            if msg.get("method") == "textDocument/publishDiagnostics":
                diagnostics = msg["params"]["diagnostics"]
        assert diagnostics == [], diagnostics

        text_doc = {"uri": URI}
        math_dot = position_of("math.sqrt")
        math_dot["character"] += len("math.")
        completion = request(proc, counter, "textDocument/completion", {
            "textDocument": text_doc,
            "position": math_dot,
        })
        labels = [item["label"] for item in completion["result"]["items"]]
        assert "sqrt" in labels, labels[:20]

        hover_pos = position_of("print(input)")
        hover = request(proc, counter, "textDocument/hover", {
            "textDocument": text_doc,
            "position": hover_pos,
        })
        assert "print" in hover["result"]["contents"]["value"]

        symbols = request(proc, counter, "textDocument/documentSymbol", {"textDocument": text_doc})
        symbol_names = [item["name"] for item in symbols["result"]]
        assert "add" in symbol_names, symbol_names
        assert "PlayerData" in symbol_names, symbol_names
        assert "host_score" in symbol_names, symbol_names

        add_use = position_of("add(1, 2)")
        definition = request(proc, counter, "textDocument/definition", {
            "textDocument": text_doc,
            "position": add_use,
        })
        assert definition["result"]["range"]["start"]["line"] == position_of("fn add")["line"]

        signature = request(proc, counter, "textDocument/signatureHelp", {
            "textDocument": text_doc,
            "position": position_of('dict.get(config') | {"character": position_of('dict.get(config')["character"] + len("dict.get(config")},
        })
        assert signature["result"]["signatures"][0]["label"] == "dict.get(dict, key, default?)"

        formatting = request(proc, counter, "textDocument/formatting", {
            "textDocument": text_doc,
            "options": {"tabSize": 4, "insertSpaces": True},
        })
        assert formatting["result"] and "newText" in formatting["result"][0]

        semantic = request(proc, counter, "textDocument/semanticTokens/full", {"textDocument": text_doc})
        assert semantic["result"]["data"], semantic

        rename = request(proc, counter, "textDocument/rename", {
            "textDocument": text_doc,
            "position": add_use,
            "newName": "sum",
        })
        assert len(rename["result"]["changes"][URI]) >= 2

        send(
            proc,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": URI, "version": 2},
                    "contentChanges": [{"text": STRICT_BAD_SOURCE}],
                },
            },
        )
        strict_diagnostics = None
        while strict_diagnostics is None:
            msg = read_message(proc)
            if msg.get("method") == "textDocument/publishDiagnostics":
                strict_diagnostics = msg["params"]["diagnostics"]
        strict_messages = [d["message"] for d in strict_diagnostics]
        assert any("Expected number" in m for m in strict_messages), strict_messages
        assert any("Cannot call number" in m for m in strict_messages), strict_messages

        shutdown = request(proc, counter, "shutdown", None)
        assert shutdown["result"] is None
        send(proc, {"jsonrpc": "2.0", "method": "exit"})
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        proc.wait(timeout=5)
        if proc.returncode not in (0, None):
            sys.stderr.write(proc.stderr.read().decode("utf-8", "replace"))
            raise SystemExit(proc.returncode)


if __name__ == "__main__":
    main()
