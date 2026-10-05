import pathlib
import subprocess
import sys
import tempfile


zapc = str(pathlib.Path(sys.argv[1]).resolve())
invalid = [
    ('macro answer($q: source) expr { return syntaxExpr("42"); }\nfun main() Int { return answer!{' + 'x' * 200_000 + '}; }', "M3003"),
    ('macro broken($q: source) expr { var = ; }\nfun main() Int { return 0; }', None),
    ('macro broken($q: source) expr { var x: String = 42; return syntaxExpr("0"); }\nfun main() Int { return 0; }', "M3002"),
    ('macro broken($q: source) expr { if false { readFile("bad"); } return syntaxExpr("0"); }\nfun main() Int { return 0; }', "M3002"),
    ('macro broken($q: source) expr { return syntaxItem(""); }\nfun main() Int { return 0; }', "M3002"),
    ('macro answer($q: source) expr { return syntaxExpr("42"); }\nfun main() Int { return answer!{${1 + }}; }', "M1002"),
    ('macro ignore($q: tokens) { 42 }\nfun main() Int { return ignore!(unknown!{${1 + }}); }', "M1002"),
    ('macro answer($q: source) expr { return syntaxExpr("42"); }\nfun main() Int { return answer!{${other!{${1 + }}}}; }', "M1002"),
    ('@ctfe fun unused() Int { return "bad"; }\nfun main() Int { return 0; }', "M3002"),
    ('fun helper() Int { if false { readFile("bad"); } return 42; }\nmacro unused($q: source) expr { if helper() == 42 { return syntaxExpr("42"); } return syntaxExpr("0"); }\nfun main() Int { return 0; }', "M3002"),
    ('fun helper(q: SyntaxSource) SyntaxExpr { return sourceInterpolation(q, 0); }\nmacro unused($q: source) expr { return helper(q); }\nfun main() Int { return 0; }', "M3002"),
    ('macro generate() { @ctfe fun hidden() Int { return "bad"; } }\ngenerate!()\nfun main() Int { return 0; }', "M1005"),
    ('import "helper" as h;\nmacro unused($q: source) expr { return h.hidden(); }\nfun main() Int { return 0; }', "M3002"),
    ('import "helper" as h { hidden };\nmacro unused($q: source) expr { return hidden(); }\nfun main() Int { return 0; }', "M3002"),
]
with tempfile.TemporaryDirectory(prefix="zap-ctfe-validation-") as directory:
    source = pathlib.Path(directory) / "main.zp"
    helper = pathlib.Path(directory) / "helper.zp"
    helper.write_text('@ctfe fun hidden() SyntaxExpr { return syntaxExpr("42"); }')
    output = pathlib.Path(directory) / "output"
    for index, (text, code) in enumerate(invalid):
        source.write_text(text)
        for mode in ([], ["--emit-expanded"]):
            result = subprocess.run(
                [zapc, "-noprelude", *mode, str(source), "-o", str(output)],
                capture_output=True, text=True, timeout=30,
            )
            assert result.returncode == 1, (index, mode, result.returncode, result.stderr[:500])
            assert not output.exists(), (index, mode, "output on invalid definition")
            if code:
                assert code in result.stderr, (index, mode, result.stderr[:500])
    source.write_text('@ctfe fun helper() Int { return 42; }\nfun main() Int { helper(); return 0; }')
    result = subprocess.run([zapc, "-noprelude", str(source), "-o", str(output)],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 1, (result.returncode, result.stderr[:500])
    assert "Cannot use @ctfe function" in result.stderr, result.stderr
    assert not output.exists(), "output on runtime call to @ctfe helper"
    source.write_text('macro unused($q: source) expr { panic("do not execute"); }\nfun main() Int { return 0; }')
    result = subprocess.run([zapc, "-noprelude", str(source), "-o", str(output)],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    assert subprocess.run([str(output)], timeout=10).returncode == 0
