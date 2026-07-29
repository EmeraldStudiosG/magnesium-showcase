#!/usr/bin/env python3
import json
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
default_binary = ROOT / ("magnesium.exe" if os.name == "nt" else "magnesium")
BIN = Path(os.environ.get("MAGNESIUM_BIN", default_binary))
URI = (ROOT / "magnesium_lsp_smoke.mg").resolve().as_uri()
UNICODE_MARKER = "\U0001F9EA"

SOURCE = """let input = input("score")
if input > 10 then
    print(input)
end
print("__UNICODE_MARKER__", input)

type PlayerData = &< name: string, score: number >
extern fn host_score(): number

fn add(a, b)
    return a + b
end

let config = &< port = 8080 >
print(dict.get(config, "port"))
print(math.sqrt(4))
print(add(1, 2))
""".replace("__UNICODE_MARKER__", UNICODE_MARKER)

STRICT_BAD_SOURCE = """!strict
let score: number = "bad"
score()
"""


def send(proc, payload):
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    send_raw(proc, body)


def send_raw(proc, body):
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
    prefix = SOURCE[line_start:before]
    return {"line": line, "character": len(prefix.encode("utf-16-le")) // 2}


def wait_for_diagnostics(proc):
    while True:
        msg = read_message(proc)
        if msg.get("method") == "textDocument/publishDiagnostics":
            return msg["params"]["diagnostics"]


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
        assert init["result"]["serverInfo"]["version"] == "1.1.1"
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

        diagnostics = wait_for_diagnostics(proc)
        assert diagnostics == [], diagnostics

        text_doc = {"uri": URI}

        # Malformed messages must be rejected without reading past their
        # Content-Length allocation or terminating the server. These cases
        # end immediately after an escape prefix, including a truncated
        # surrogate pair.
        malformed_change_prefix = (
            b'{"jsonrpc":"2.0","method":"textDocument/didChange","params":'
            b'{"textDocument":{"uri":'
            + json.dumps(URI).encode("utf-8")
            + b'},"contentChanges":[{"text":"'
        )
        for escape_suffix in (
            b"\\",
            b"\\u12",
            b"\\uD800",
            b"\\uD800\\u12",
        ):
            send_raw(proc, malformed_change_prefix + escape_suffix)

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

        unicode_hover_pos = position_of(f'print("{UNICODE_MARKER}", input)')
        unicode_prefix = f'print("{UNICODE_MARKER}", '
        unicode_hover_pos["character"] += len(unicode_prefix.encode("utf-16-le")) // 2
        unicode_hover = request(proc, counter, "textDocument/hover", {
            "textDocument": text_doc,
            "position": unicode_hover_pos,
        })
        assert unicode_hover["result"]["range"]["start"]["character"] == (
            len(unicode_prefix.encode("utf-16-le")) // 2
        )

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
        strict_diagnostics = wait_for_diagnostics(proc)
        strict_messages = [d["message"] for d in strict_diagnostics]
        assert any("Expected number" in m for m in strict_messages), strict_messages
        assert any("Cannot call number" in m for m in strict_messages), strict_messages

        # Regression coverage for response builders that used to truncate or
        # advance past fixed-size buffers on sufficiently large documents.
        large_source = "\n".join(f"let value_{i} = {i}" for i in range(7000)) + "\n"
        send(
            proc,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": URI, "version": 3},
                    "contentChanges": [{"text": large_source}],
                },
            },
        )
        wait_for_diagnostics(proc)
        semantic = request(
            proc,
            counter,
            "textDocument/semanticTokens/full",
            {"textDocument": text_doc},
        )
        assert len(semantic["result"]["data"]) >= 7000
        symbols = request(
            proc,
            counter,
            "textDocument/documentSymbol",
            {"textDocument": text_doc},
        )
        assert len(symbols["result"]) == 7000
        formatting = request(
            proc,
            counter,
            "textDocument/formatting",
            {"textDocument": text_doc, "options": {"tabSize": 4, "insertSpaces": True}},
        )
        assert formatting["result"][0]["newText"].count("\n") == 7000

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
