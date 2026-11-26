# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from pathlib import Path
import os
import stat
import subprocess
import sys
import tempfile


def invoke(tool, *args, status=0, message=None):
    result = subprocess.run([tool, *args], capture_output=True)
    assert result.returncode == status, (args, result.returncode, result.stderr)
    if message:
        assert message.encode() in result.stderr, (message, result.stderr)
    return result.stdout


def check(tool, path, original, expected, ambiguous=None):
    path.write_bytes(original.encode())
    if ambiguous:
        invoke(tool, path, status=1, message=ambiguous)
        assert path.read_bytes() == original.encode()
    invoke(tool, "--from-legacy", path)
    assert path.read_bytes() == expected.encode(), (path, expected, path.read_bytes())
    # Catch a second publication even if it writes identical bytes.
    os.utime(path, (1_700_000_000, 1_700_000_000))
    modified = path.stat().st_mtime_ns
    for flags in [("--from-legacy",), ()]:
        assert invoke(tool, *flags, path).startswith(b"unchanged ")
        assert path.read_bytes() == expected.encode()
        assert path.stat().st_mtime_ns == modified


def main(tool):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        path = root / "Café sample.co"
        wrapped = "pub fn<int> main() {\r\n    def n := 7 // café\r\n        - 2;\r\n    return n;\r\n}"
        check(tool, path, wrapped, wrapped.replace("7 //", "7; //"), "2:14-2:15 Ambiguous")
        check(tool, path, "fn f() { return\nwork(); }", "fn f() { return;\nwork(); }", "Ambiguous")
        check(tool, path, "fn<int> pick(bool b) = pair(if (b) 4 else 2, 3);",
              "fn<int> pick(bool b) = pair(if (b) 4 else { 2, 3; });", "Ambiguous")
        check(tool, path, "fn<int,int> choose(bool b) = if (b) 4, 3 else 2, 1",
              "fn<int,int> choose(bool b) = if (b) { 4, 3; } else { 2, 1; };")
        check(tool, path, "fn f() { ;; for (int i <- 0..3; i++) { go();; }\n"
              "if (a) if (b) start() else stop()\ndo work() while (ready)\n}",
              "fn f() {  for (int i <- 0..3; i++) { go(); }\n"
              "if (a) if (b) start(); else stop();\ndo work(); while (ready);\n}")
        check(tool, path, "fn f() { if (a) { start() } else { stop() }\n-2 }",
              "fn f() { (if (a) { start(); } else { stop(); });\n-2; }")
        check(tool, path, "fn f() { Point p := {1, 2}\nint a, b := { return (42) } // both\n}",
              "fn f() { Point p := {1, 2};\nint a, b := { return (42); }; // both\n}")
        check(tool, path, "fn f() { report(\"a;{b}\") /* keep */\n; next() }",
              "fn f() { report(\"a;{b}\"); /* keep */\n next(); }")
        library = root / "types.clib"
        check(tool, library, "using A: B\ntypedef<class> Empty {}\ntypedef<class> Box(pub def<int> n)\n"
              "typedef<struct> Point { int x, int y }\ntypedef<enum> Flag { Yes, No }",
              "using A: B;\ntypedef<class> Empty {}\ntypedef<class> Box(pub def<int> n);\n"
              "typedef<struct> Point { int x, int y; }\ntypedef<enum> Flag { Yes, No; }")
        check(tool, library, "pub typedef<class> MyClass;", "pub typedef<class> MyClass;")

        # An empty, explicitly deferred named-type initializer has no equivalent strict spelling.
        for source, message in [("fn f() { Box x := {;} }", "changes the legacy parse"),
                                ("fn broken(", "Cannot parse source")]:
            path.write_text(source, encoding="utf-8")
            invoke(tool, "--from-legacy", path, status=1, message=message)
            assert path.read_text(encoding="utf-8") == source

        # Per-file publication still converts valid siblings and ignores non-source files.
        library.write_bytes(b"fn<int> answer() = 42")
        other = root / "notes.txt"
        other.write_bytes(b"text")
        invoke(tool, root, status=1, message="Cannot parse source")
        assert library.read_bytes() == b"fn<int> answer() = 42;"
        assert other.read_bytes() == b"text"

        library.write_bytes(b"fn answer() = return")
        if os.name == "nt":
            library.chmod(stat.S_IREAD)
            try:
                invoke(tool, "--from-legacy", library, status=1)
                assert library.read_bytes() == b"fn answer() = return"
                assert set(root.iterdir()) == {path, library, other}
            finally:
                library.chmod(stat.S_IWRITE)
        else:
            library.chmod(0o750)
            invoke(tool, "--from-legacy", library)
            assert stat.S_IMODE(library.stat().st_mode) == 0o750


if __name__ == "__main__":
    main(sys.argv[1])
