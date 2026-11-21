# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from pathlib import Path
import os
import subprocess
import sys


def limit_file_size():
    import resource
    import signal

    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (1, 1))


def main():
    compiler, source, output_name = sys.argv[1:]
    output = Path(output_name)
    previous = output.read_bytes()
    files = set(output.parent.iterdir())
    # An open file denies replacement on Windows. On POSIX, limit the temporary file's size.
    with output.open("rb"):
        result = subprocess.run(
            [compiler, "--kind", "executable", "--src", source, "--emit-object", output],
            capture_output=True, preexec_fn=limit_file_size if os.name == "posix" else None,
        )
    if result.returncode != 1 or result.stdout or b"[C0013]" not in result.stderr:
        sys.exit(f"Expected an object write error, got {result.returncode}, {result.stdout!r}, {result.stderr!r}")
    if output.read_bytes() != previous or set(output.parent.iterdir()) != files:
        sys.exit("Failed object write changed the destination or left a temporary file")


if __name__ == "__main__":
    main()
