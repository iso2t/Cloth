# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
import os
from pathlib import Path
import shlex
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
    parser.add_argument("--runtime", type=Path, help="Packaged runtime archive used to link native fixtures")
    parser.add_argument("--readobj", default="llvm-readobj")
    parser.add_argument("--llvm-mc", default="llvm-mc")
    parser.add_argument("--cc", nargs=argparse.REMAINDER, help="C driver and flags used to link native objects")
    args = parser.parse_args()

    suite = Path(__file__).resolve().parents[1]
    selection = args.selection.resolve()
    if args.single and (not selection.is_file() or suite not in selection.parents):
        parser.error("test-file requires CASE=path/to/fixture naming one file under Tests/")

    lit = find_tool("LIT", "llvm-lit", "lit")
    filecheck = find_tool("FILECHECK", "FileCheck")
    cc = args.cc or ["gcc" if os.name == "nt" else "cc"]
    cc_path = Path(shutil.which(cc[0]) or cc[0]).as_posix()
    compiler = args.compiler.resolve()
    runtime = args.runtime
    if runtime is None:
        triple = subprocess.check_output([compiler, "--print-target"], text=True).strip()
        runtime = compiler.parent / "lib" / "cloth" / triple / "libcloth_runtime.a"
    output = compiler.parent / "Tests"
    output.mkdir(exist_ok=True)
    run_dir = tempfile.mkdtemp(prefix="run-", dir=output)
    command = [
        lit, "-v",
        "-D", f"cloth={compiler}",
        "-D", f"runtime={runtime.resolve()}",
        "-D", f"filecheck={filecheck}",
        "-D", f"readobj={args.readobj}",
        "-D", f"llvm_mc={args.llvm_mc}",
        "-D", f"cc={shlex.join([cc_path, *cc[1:]])}",
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
