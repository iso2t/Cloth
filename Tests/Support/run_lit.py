# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def find_tool(variable, *names):
    override = os.environ.get(variable)
    candidates = (override,) if override else names
    for candidate in candidates:
        path = shutil.which(candidate)
        if path:
            return str(Path(path).absolute())
    sys.exit(f"Could not find {' or '.join(candidates)}. Set {variable} to the executable path.")


def main():
    parser = argparse.ArgumentParser(description="Run Cloth regression tests with LLVM lit.")
    parser.add_argument("--single", action="store_true")
    parser.add_argument("compiler", type=Path)
    parser.add_argument("selection", type=Path)
    args = parser.parse_args()

    suite = Path(__file__).resolve().parents[1]
    selection = args.selection.resolve()
    if args.single and (not selection.is_file() or suite not in selection.parents):
        parser.error("test-file requires CASE=path/to/fixture naming one file under Tests/")

    lit = find_tool("LIT", "llvm-lit", "lit")
    filecheck = find_tool("FILECHECK", "FileCheck")
    compiler = args.compiler.resolve()
    output = compiler.parent / "Tests"
    output.mkdir(exist_ok=True)
    run_dir = tempfile.mkdtemp(prefix="run-", dir=output)
    command = [
        lit, "-v",
        "-D", f"cloth={compiler}",
        "-D", f"filecheck={filecheck}",
        "-D", f"exec_root={run_dir}",
    ]
    if args.single:
        command.extend(["-D", f"case={selection.relative_to(suite).as_posix()}"])
    result = subprocess.run([*command, str(selection)])
    if result.returncode:
        print(f"Test output retained in {run_dir}", file=sys.stderr)
    else:
        shutil.rmtree(run_dir)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
