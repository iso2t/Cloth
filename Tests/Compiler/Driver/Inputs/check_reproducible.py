# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from pathlib import Path
import subprocess
import sys


def main():
    compiler, stdlib, source, prefix = sys.argv[1:]
    for bits in ("32", "64"):
        for kind, extension in (("object", "o"), ("assembly", "s")):
            output = Path(f"{prefix}.{bits}.{extension}")
            command = [compiler, "--kind", "executable", "--stdlib", stdlib, "--src", source,
                       "--target-bits", bits, "--emit", kind, "-o", output]
            subprocess.run(command, check=True)
            first = output.read_bytes()
            for attempt in range(2, 9):
                subprocess.run(command, check=True)
                if output.read_bytes() != first:
                    output.with_suffix(f".first.{extension}").write_bytes(first)
                    sys.exit(f"{source}: {bits}-bit {kind} changed on compilation {attempt}")


if __name__ == "__main__":
    main()
