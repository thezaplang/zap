import pathlib
import subprocess
import sys
import tempfile


zapc = str(pathlib.Path(sys.argv[1]).resolve())
parens = "(" * 4000 + "1" + ")" * 4000
unary = "!" * 4000 + "true"
generated_unary = "!" * 513 + "true"
generated_parens = "(" * 257 + "1" + ")" * 257
cases = [
    f"fun main() Int {{ return {parens}; }}",
    "macro identity($x: expr) { $x }\n"
    f"fun main() Int {{ return identity!({parens}); }}",
    f"fun main() Bool {{ return {unary}; }}",
    "alias Deep = " + "*" * 4000 + "Int;",
    "fun main() { iftype T == Int {}" + " else iftype T == Int {}" * 4000 + " }",
    "fun main() { return Value" + ".field" * 4000 + "{}; }",
    "fun main() Int { return 1" + " + 1" * 4000 + "; }",
    'macro generate($x: tokens) expr { return syntaxExpr("'
    + generated_unary
    + '"); }\nfun main() Bool { return generate!(1); }',
    'macro generate($x: tokens) expr { return syntaxExpr("'
    + generated_parens
    + '"); }\nfun main() Int { return generate!(1); }',
]
with tempfile.TemporaryDirectory(prefix="zap-syntax-nesting-") as directory:
    source = pathlib.Path(directory) / "main.zp"
    output = pathlib.Path(directory) / "output"
    for index, text in enumerate(cases):
        source.write_text(text)
        for mode in ([], ["--emit-expanded"]):
            result = subprocess.run(
                [zapc, "-noprelude", *mode, str(source), "-o", str(output)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            assert result.returncode > 0, (index, mode, result.returncode)
            assert "nesting limit exceeded" in result.stderr.lower(), (
                index, mode, result.stderr[:500]
            )
            assert not output.exists(), (index, mode, "output written on error")
