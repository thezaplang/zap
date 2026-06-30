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


def completion_items(proc, uri, line, character, request_id):
    response = request(
        proc,
        "textDocument/completion",
        {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}},
        request_id,
    )
    if "error" in response:
        raise AssertionError(response["error"])
    return response["result"]


def signature_help(proc, uri, line, character, request_id):
    response = request(
        proc,
        "textDocument/signatureHelp",
        {
            "textDocument": {"uri": uri},
            "position": {"line": line, "character": character},
        },
        request_id,
    )
    if "error" in response:
        raise AssertionError(response["error"])
    return response["result"]


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
    env["ZAPC_CORE_DIR"] = str(ROOT / "core")
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

            nested_member_source = """class Counter {
    pub fun inc(step: Int) Int {
        return step;
    }
}

fun main() Int {
    if true {
        var counter: Counter = new Counter();
        counter.
    }
    return 0;
}
"""
            nested_uri = open_document(
                proc, temp / "nested_member.zp", nested_member_source
            )
            labels = completion_labels(proc, nested_uri, 9, 16, 4)
            assert (
                "inc" in labels
            ), "member completion missing for local class variable in nested block"

            inferred_member_source = """class Counter {
    pub fun inc(step: Int) Int {
        return step;
    }
}

fun main() Int {
    var counter = new Counter();
    counter.inc(1);
    return 0;
}
"""
            inferred_uri = open_document(
                proc, temp / "inferred_member.zp", inferred_member_source
            )
            labels = completion_labels(proc, inferred_uri, 8, 12, 5)
            assert (
                "inc" in labels
            ), "member completion missing for inferred local class variable"

            member_prefix_items = completion_items(proc, inferred_uri, 8, 13, 6)
            member_prefix_labels = {item["label"] for item in member_prefix_items}
            assert (
                "inc" in member_prefix_labels
            ), "member completion missing after partially typed member name"
            assert (
                "return" not in member_prefix_labels
            ), "member completion leaked keyword suggestions"
            assert (
                "Counter" not in member_prefix_labels
            ), "member completion leaked top-level symbols"

            record_member_source = """record test {
    a: Int
}

fun main() Int {
    var b: test = test{a: 5};
    b.
    return 0;
}
"""
            record_uri = open_document(
                proc, temp / "record_member.zp", record_member_source
            )
            labels = completion_labels(proc, record_uri, 6, 6, 7)
            assert "a" in labels, "record field missing from member completion"

            generic_class_source = """fun main() Int {
    var a = new List<String>();
    a.
    return 0;
}
"""
            generic_uri = open_document(
                proc, temp / "generic_class_member.zp", generic_class_source
            )
            labels = completion_labels(proc, generic_uri, 2, 6, 8)
            assert "len" in labels, "generic class method missing from member completion"
            assert "push" in labels, "generic class method missing from member completion"

            struct_literal_source = """struct test {
    name: String,
    age: Int16
}

fun main() Int {
    var a = test{};
    return 0;
}
"""
            struct_literal_uri = open_document(
                proc, temp / "struct_literal_completion.zp", struct_literal_source
            )
            labels = completion_labels(proc, struct_literal_uri, 6, 17, 9)
            assert "name" in labels, "struct literal field missing from completion"
            assert "age" in labels, "struct literal field missing from completion"
            assert "return" not in labels, "struct literal completion leaked keywords"

            record_literal_source = """record test {
    name: String,
    age: Int16
}

fun main() Int {
    var a = test{};
    return 0;
}
"""
            record_literal_uri = open_document(
                proc, temp / "record_literal_completion.zp", record_literal_source
            )
            labels = completion_labels(proc, record_literal_uri, 6, 17, 10)
            assert "name" in labels, "record literal field missing from completion"
            assert "age" in labels, "record literal field missing from completion"
            assert "return" not in labels, "record literal completion leaked keywords"

            imported_module_source = """import "std/convert";

fun main() Int {
   convert.
   return 0;
}
"""
            imported_module_uri = open_document(
                proc, temp / "imported_module_completion.zp", imported_module_source
            )
            items = completion_items(proc, imported_module_uri, 3, 11, 11)
            labels = [item["label"] for item in items]
            assert labels.count("toInt") == 1, "overloaded imported member duplicated in completion"

            constructor_source = """class Counter {
    fun init(value: Int) {
    }

    fun init(value: String, repeat: Int) {
    }
}

fun newCounter(value: Float) Int {
    return 0;
}

fun main() Int {
    var counter = new Counter(1, );
    var value = newCounter(1.0);
    return 0;
}
"""
            constructor_uri = open_document(
                proc, temp / "constructor_signature.zp", constructor_source
            )
            signatures = signature_help(proc, constructor_uri, 13, 33, 12)
            assert signatures is not None, "constructor signature help is missing"
            labels = {item["label"] for item in signatures["signatures"]}
            assert labels == {
                "init(value: Int) Void",
                "init(value: String, repeat: Int) Void",
            }, "constructor overloads are missing from signature help"
            assert signatures["activeParameter"] == 1

            signatures = signature_help(proc, constructor_uri, 14, 27, 13)
            assert signatures is not None, "newCounter signature help is missing"
            assert [item["label"] for item in signatures["signatures"]] == [
                "newCounter(value: Float) Int"
            ], "newCounter was incorrectly resolved as a constructor"

        request(proc, "shutdown", None, 14)
        notify(proc, "exit", {})
        proc.wait(timeout=5)
    finally:
        if proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    main()
