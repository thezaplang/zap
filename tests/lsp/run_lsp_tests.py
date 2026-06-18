#!/usr/bin/env python3
import json
import os
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SERVER = ROOT / "build" / "zap-lsp"


def send(proc, payload):
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    proc.stdin.write(b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n\r\n")
    proc.stdin.write(body)
    proc.stdin.flush()


def read_message(proc):
    headers = {}
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("zap-lsp exited before sending a response")
        line = line.decode("ascii")
        if line in ("\r\n", "\n", ""):
            break
        key, value = line.split(":", 1)
        headers[key.strip().lower()] = value.strip()

    length = int(headers["content-length"])
    return json.loads(proc.stdout.read(length).decode("utf-8"))


def request(proc, method, params, request_id):
    send(
        proc,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
            "params": params,
        },
    )
    while True:
        response = read_message(proc)
        if response.get("id") == request_id:
            return response


def notify(proc, method, params):
    send(proc, {"jsonrpc": "2.0", "method": method, "params": params})


def file_uri(path):
    return pathlib.Path(path).resolve().as_uri()


def completion_labels(proc, uri, line, character, request_id):
    response = request(
        proc,
        "textDocument/completion",
        {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}},
        request_id,
    )
    if "error" in response:
        raise AssertionError(response["error"])
    return {item["label"] for item in response["result"]}


def open_document(proc, path, text):
    uri = file_uri(path)
    pathlib.Path(path).write_text(text)
    notify(
        proc,
        "textDocument/didOpen",
        {
            "textDocument": {
                "uri": uri,
                "languageId": "zap",
                "version": 1,
                "text": text,
            }
        },
    )
    return uri


def main():
    if not SERVER.exists():
        raise SystemExit(f"missing {SERVER}; build zap-lsp first")

    env = os.environ.copy()
    env["ZAPC_STDLIB_DIR"] = str(ROOT / "std")
    proc = subprocess.Popen(
        [str(SERVER)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )

    try:
        init = request(
            proc,
            "initialize",
            {"processId": None, "rootUri": file_uri(ROOT), "capabilities": {}},
            1,
        )
        assert "capabilities" in init["result"]
        notify(proc, "initialized", {})

        with tempfile.TemporaryDirectory(prefix="zap-lsp-") as temp_dir:
            temp = pathlib.Path(temp_dir)

            loop_source = """fun main() Int {
    var values: [2]Int = {1, 2};
    for v in values {
        v
    }
    return 0;
}
"""
            loop_uri = open_document(proc, temp / "loop.zp", loop_source)
            labels = completion_labels(proc, loop_uri, 3, 9, 2)
            assert "v" in labels, "for-in item variable missing from completion"

            class_source = """class Counter {
    priv value: Int;
    pub fun inc(step: Int) Int {
        st
    }
}
"""
            class_uri = open_document(proc, temp / "counter.zp", class_source)
            labels = completion_labels(proc, class_uri, 3, 10, 3)
            assert "step" in labels, "method parameter missing from completion"
            assert "value" in labels, "class field missing from method completion"

        request(proc, "shutdown", None, 4)
        notify(proc, "exit", {})
        proc.wait(timeout=5)
    finally:
        if proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    main()
