# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from pathlib import Path
import subprocess
import sys


compiler, directory = sys.argv[1:]
for name, ending in (("lf", b"\n"), ("crlf", b"\r\n"), ("cr", b"\r")):
    source = Path(directory) / f"{name}.co"
    command = [compiler, "--kind", "executable", "--src", str(source)]
    source.write_bytes(b"// heading" + ending + b"pub fn<int> main() = 42;" + ending)
    subprocess.run(command, check=True)

    source.write_bytes(b"// heading" + ending + b"$" + ending)
    result = subprocess.run(command, capture_output=True)
    expected = (b"[C1000]", f"{source.name}:2:1 ".encode(), b"2 | $")
    if result.returncode != 1 or not all(part in result.stderr for part in expected):
        raise AssertionError(f"{name}: expected the invalid token at 2:1, got {result.returncode}: {result.stderr!r}")
