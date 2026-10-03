import pathlib
import subprocess
import sys
import tempfile


zapc = str(pathlib.Path(sys.argv[1]).resolve())
invalid = [
    ('macro broken($q: source) expr { var = ; }\nfun main() Int { return 0; }', None),
    ('macro broken($q: source) expr { var x: String = 42; return syntaxExpr("0"); }\nfun main() Int { return 0; }', "M3002"),
    ('macro broken($q: source) expr { if false { readFile("bad"); } return syntaxExpr("0"); }\nfun main() Int { return 0; }', "M3002"),
    ('macro broken($q: source) expr { return syntaxItem(""); }\nfun main() Int { return 0; }', "M3002"),
    ('macro answer($q: source) expr { return syntaxExpr("42"); }\nfun main() Int { return answer!{${1 + }}; }', "M1002"),
    ('macro ignore($q: tokens) { 42 }\nfun main() Int { return ignore!(unknown!{${1 + }}); }', "M1002"),
    ('macro answer($q: source) expr { return syntaxExpr("42"); }\nfun main() Int { return answer!{${other!{${1 + }}}}; }', "M1002"),
]
with tempfile.TemporaryDirectory(prefix="zap-ctfe-validation-") as directory:
    source = pathlib.Path(directory) / "main.zp"
    output = pathlib.Path(directory) / "output"
    for index, (text, code) in enumerate(invalid):
        source.write_text(text)
        for mode in ([], ["--emit-expanded"]):
            result = subprocess.run(
                [zapc, "-noprelude", *mode, str(source), "-o", str(output)],
                capture_output=True, text=True, timeout=30,
            )
            assert result.returncode > 0, (index, mode, result.returncode)
            assert not output.exists(), (index, mode, "output on invalid definition")
            if code:
                assert code in result.stderr, (index, mode, result.stderr[:500])
    source.write_text('macro unused($q: source) expr { panic("do not execute"); }\nfun main() Int { return 0; }')
    result = subprocess.run([zapc, "-noprelude", str(source), "-o", str(output)],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    assert subprocess.run([str(output)], timeout=10).returncode == 0
