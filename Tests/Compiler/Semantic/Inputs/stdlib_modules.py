# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from pathlib import Path
import subprocess
import sys


def main():
    compiler, stdlib, consumers = map(Path, sys.argv[1:])
    entry = stdlib / "cloth" / "Library.co"
    sources = [entry] + sorted(
        path for path in entry.parent.rglob("*")
        if path != entry and path.suffix.lower() in {".co", ".clib"} and path.is_file()
    )
    consumers.mkdir(parents=True, exist_ok=True)

    failed = False
    for index, path in enumerate(sources):
        relative = path.relative_to(stdlib)
        source = path
        if path != entry:
            module = ".".join(relative.with_suffix("").parts)
            source = consumers / f"import-{index}.co"
            source.write_text(f"using {module}: *;\n", encoding="utf-8")

        for bits in ("32", "64"):
            result = subprocess.run(
                [compiler, "--kind", "library", "--stdlib", stdlib, "--target-bits", bits, "--src", source],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8", errors="replace",
            )
            if result.returncode or result.stdout:
                print(f"{relative.as_posix()} ({bits}-bit target, exit {result.returncode})", file=sys.stderr)
                if result.stdout:
                    print(result.stdout.rstrip("\n"), file=sys.stderr)
                failed = True

    if failed:
        return 1
    print(f"Checked {len(sources)} Stdlib sources on both 32-bit and 64-bit targets.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
