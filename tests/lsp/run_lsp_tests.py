#!/usr/bin/env python3
import json
import os
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SERVER = pathlib.Path(
    os.environ.get("ZAP_LSP_SERVER", ROOT / "build" / "zap-lsp")
).resolve()


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


def read_response(proc, request_id):
    while True:
        response = read_message(proc)
        if response.get("id") == request_id:
            return response


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
    return read_response(proc, request_id)


def notify(proc, method, params):
    send(proc, {"jsonrpc": "2.0", "method": method, "params": params})


def watched_files_changed(proc, paths):
    notify(
        proc,
        "workspace/didChangeWatchedFiles",
        {
            "changes": [
                {"uri": file_uri(path), "type": 2}
                for path in paths
            ]
        },
    )


def workspace_folders_changed(proc, folder):
    notify(
        proc,
        "workspace/didChangeWorkspaceFolders",
        {
            "event": {
                "added": [{"uri": file_uri(folder), "name": folder.name}],
                "removed": [],
            }
        },
    )


def read_diagnostics(proc, expected_uri):
    while True:
        message = read_message(proc)
        if message.get("method") != "textDocument/publishDiagnostics":
            continue
        params = message["params"]
        if params["uri"] == expected_uri:
            return params["diagnostics"]


def file_uri(path):
    return pathlib.Path(path).resolve().as_uri()


def utf16_length(text):
    return len(text.encode("utf-16-le")) // 2


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


def definition_location(proc, uri, line, character, request_id):
    response = request(
        proc,
        "textDocument/definition",
        {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}},
        request_id,
    )
    if "error" in response:
        raise AssertionError(response["error"])
    return response["result"]


def hover(proc, uri, line, character, request_id):
    response = request(
        proc,
        "textDocument/hover",
        {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}},
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


def change_document(proc, uri, text, version):
    notify(
        proc,
        "textDocument/didChange",
        {
            "textDocument": {"uri": uri, "version": version},
            "contentChanges": [{"text": text}],
        },
    )


def close_document(proc, uri):
    notify(proc, "textDocument/didClose", {"textDocument": {"uri": uri}})


def main():
    if not SERVER.exists():
        raise SystemExit(f"missing {SERVER}; build zap-lsp first")

    with tempfile.TemporaryDirectory(prefix="zap-lsp-workspace-") as workspace_dir:
        workspace_root = pathlib.Path(workspace_dir)
        (workspace_root / "thor.toml").write_text(
            """entry = "src/main.zp"

[imports]
"@vendor" = "./vendor/package"
            """
        )
        alternate_project = workspace_root / "alternate"
        alternate_project.mkdir()
        (alternate_project / "thor.toml").write_text(
            """entry = "src/main.zp"

[imports]
"@alternate" = "./packages/alternate"
"""
        )
        proc = subprocess.Popen(
            [str(SERVER)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            init = request(
                proc,
                "initialize",
                {
                    "processId": None,
                    "rootUri": file_uri(workspace_root),
                    "capabilities": {},
                },
                1,
            )
            assert "capabilities" in init["result"]
            assert init["result"]["serverInfo"]["version"] == "0.1.0"
            assert init["result"]["capabilities"]["workspace"][
                "workspaceFolders"
            ] == {"supported": True, "changeNotifications": True}
            notify(proc, "initialized", {})
            temp = workspace_root

            unknown_method = request(proc, "zap/unknown", {}, 90)
            assert unknown_method["error"]["code"] == -32601, (
                "unknown request did not return MethodNotFound"
            )
            invalid_completion = request(
                proc,
                "textDocument/completion",
                {"textDocument": {"uri": file_uri(temp / "missing.zp")}},
                91,
            )
            assert invalid_completion["error"]["code"] == -32602, (
                "malformed completion did not return InvalidParams"
            )

            cancellation_source = "\n".join(
                f"fun queued{i}() Int {{ return {i}; }}" for i in range(2000)
            )
            cancellation_uri = open_document(
                proc, temp / "cancellation.zp", cancellation_source
            )
            send(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 92,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": cancellation_uri},
                        "position": {"line": 0, "character": 3},
                    },
                },
            )
            notify(proc, "$/cancelRequest", {"id": 92})
            cancelled = read_response(proc, 92)
            assert cancelled["error"]["code"] == -32800, (
                "cancelled request published a normal result"
            )
            reused_id = request(proc, "zap/unknown", {}, 92)
            assert reused_id["error"]["code"] == -32601, (
                "cancellation leaked into a later request with the same id"
            )

            imported_path = temp / "broken.zp"
            imported_path.write_text("fun broken() Int {\n")
            imported_uri = file_uri(imported_path)
            main_with_broken_import = """import "broken.zp";

fun main() Int {
    return 0;
}
"""
            open_document(
                proc, temp / "main_with_broken_import.zp", main_with_broken_import
            )
            diagnostics = read_diagnostics(proc, imported_uri)
            assert diagnostics, "imported-file diagnostic was not published"

            missing_import_source = """import "does_not_exist.zp";

fun main() Int {
    var retained: Int = 1;
    retained;
    return 0;
}
"""
            missing_import_uri = open_document(
                proc, temp / "missing_import.zp", missing_import_source
            )
            assert read_diagnostics(proc, missing_import_uri), (
                "missing import did not produce a diagnostic on its declaration"
            )
            labels = completion_labels(proc, missing_import_uri, 4, 12, 93)
            assert "retained" in labels, (
                "missing import prevented completion for an independent local symbol"
            )

            open_document(
                proc,
                imported_path,
                """fun broken() Int {
    return 0;
}
""",
            )
            assert read_diagnostics(proc, imported_uri) == [], (
                "resolved diagnostics were not cleared for the imported file"
            )

            alias_module_path = temp / "vendor" / "package" / "answer.zp"
            alias_module_path.parent.mkdir(parents=True, exist_ok=True)
            alias_module_path.write_text(
                """pub fun answer() Int {
    return 42;
}
"""
            )
            alias_source = """import "@vendor/answer";

fun main() Int {
    return answer();
}
"""
            alias_uri = open_document(proc, temp / "thor_import_map.zp", alias_source)
            assert read_diagnostics(proc, alias_uri) == [], (
                "thor.toml import map produced diagnostics"
            )

            (temp / "thor.toml").write_text("entry =")
            workspace_folders_changed(proc, temp)
            assert read_diagnostics(proc, alias_uri), (
                "changing workspace folders did not refresh cached Thor configuration"
            )

            (temp / "thor.toml").write_text(
                """entry = "src/main.zp"

[imports]
"@vendor" = "./vendor/package"
"""
            )
            workspace_folders_changed(proc, temp)
            assert read_diagnostics(proc, alias_uri) == [], (
                "workspace folder refresh did not restore Thor configuration"
            )

            legacy_project = temp / "legacy_flags"
            legacy_project.mkdir()
            legacy_module = legacy_project / "package" / "answer.zp"
            legacy_module.parent.mkdir()
            legacy_module.write_text(
                """pub fun answer() Int {
    return 42;
}
"""
            )
            (legacy_project / "zap_flags.txt").write_text(
                "--import-map @legacy=./package\n"
            )
            legacy_uri = open_document(
                proc,
                legacy_project / "main.zp",
                """import "@legacy/answer";

fun main() Int {
    return answer();
}
""",
            )
            assert read_diagnostics(proc, legacy_uri), (
                "zap_flags.txt was still used for project imports"
            )

            (temp / "thor.toml").write_text("entry =")
            watched_files_changed(proc, [temp / "thor.toml"])
            assert read_diagnostics(proc, alias_uri), (
                "changing thor.toml did not refresh dependent diagnostics"
            )

            (temp / "thor.toml").write_text(
                """entry = "src/main.zp"

[imports]
"@vendor" = "./vendor/package"
"""
            )
            watched_files_changed(proc, [temp / "thor.toml"])
            assert read_diagnostics(proc, alias_uri) == [], (
                "restoring thor.toml did not clear dependent diagnostics"
            )

            alias_module_path.write_text("fun answer() Int {\n")
            watched_files_changed(proc, [alias_module_path])
            assert read_diagnostics(proc, file_uri(alias_module_path)), (
                "changing an unopened imported file did not refresh diagnostics"
            )

            alias_module_path.write_text(
                """pub fun answer() Int {
    return 42;
}
"""
            )
            watched_files_changed(proc, [alias_module_path])
            assert read_diagnostics(proc, file_uri(alias_module_path)) == [], (
                "repairing an unopened imported file did not clear diagnostics"
            )

            alternate_module_path = (
                alternate_project / "packages" / "alternate" / "answer.zp"
            )
            alternate_module_path.parent.mkdir(parents=True)
            alternate_module_path.write_text(
                """pub fun answer() Int {
    return 7;
}
"""
            )
            alternate_source = """import "@alternate/answer";

fun main() Int {
    return answer();
}
"""
            (alternate_project / "src").mkdir()
            alternate_uri = open_document(
                proc, alternate_project / "src" / "main.zp", alternate_source
            )
            assert read_diagnostics(proc, alternate_uri) == [], (
                "nested Thor project did not use its own import map"
            )

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

            let_source = """fun main() Int {
    let value = 1;
    return value;
}
"""
            let_uri = open_document(proc, temp / "let_binding.zp", let_source)
            labels = completion_labels(proc, let_uri, 2, 4, 19)
            assert "let" in labels, "let keyword missing from completion"
            let_hover = hover(proc, let_uri, 2, 11, 20)
            assert "let value: isize" in let_hover["contents"]["value"], (
                "let hover returned unexpected information: " f"{let_hover}"
            )

            unicode_prefix = '    var note: String = "😀"; '
            unicode_source = f"""fun main() Int {{
{unicode_prefix}ret
    return 0;
}}
"""
            unicode_uri = open_document(
                proc, temp / "unicode_position.zp", unicode_source
            )
            labels = completion_labels(
                proc,
                unicode_uri,
                1,
                utf16_length(unicode_prefix + "ret"),
                15,
            )
            assert "return" in labels, "UTF-16 cursor did not reach completion"

            unicode_definition_prefix = '    var note: String = "😀"; var alias: Int = '
            unicode_definition_source = f"""fun main() Int {{
    var value: Int = 1;
{unicode_definition_prefix}value;
    return alias;
}}
"""
            unicode_definition_uri = open_document(
                proc, temp / "unicode_definition.zp", unicode_definition_source
            )
            value_position = utf16_length(unicode_definition_prefix) + 2
            location = definition_location(
                proc, unicode_definition_uri, 2, value_position, 16
            )
            assert location["uri"] == unicode_definition_uri
            assert location["range"]["start"] == {"line": 1, "character": 4}, (
                "UTF-16 cursor resolved an unexpected definition: "
                f'{location["range"]["start"]}'
            )

            hover_result = hover(
                proc, unicode_definition_uri, 2, value_position, 17
            )
            assert "var value: isize" in hover_result["contents"]["value"], (
                "UTF-16 cursor returned unexpected hover information: "
                f'{hover_result}'
            )

            lifecycle_invalid_source = "fun main() Int {\n"
            lifecycle_uri = open_document(
                proc, temp / "document_lifecycle.zp", lifecycle_invalid_source
            )
            assert read_diagnostics(proc, lifecycle_uri), (
                "invalid opened document did not publish diagnostics"
            )
            lifecycle_valid_source = """fun main() Int {
    return 0;
}
"""
            change_document(proc, lifecycle_uri, lifecycle_valid_source, 2)
            assert read_diagnostics(proc, lifecycle_uri) == [], (
                "didChange did not clear resolved diagnostics"
            )
            close_document(proc, lifecycle_uri)
            assert read_diagnostics(proc, lifecycle_uri) == [], (
                "didClose did not clear document diagnostics"
            )

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

            request(proc, "shutdown", None, 18)
            notify(proc, "exit", {})
            proc.wait(timeout=5)
        finally:
            if proc.poll() is None:
                proc.kill()


if __name__ == "__main__":
    main()
