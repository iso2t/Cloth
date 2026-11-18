# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
import os
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description="Check a native exit status and optionally require empty streams.")
    parser.add_argument("--empty-stdout", action="store_true")
    parser.add_argument("--empty-stderr", action="store_true")
    parser.add_argument("status", type=lambda value: int(value, 0))
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command:
        parser.error("expected a command after the exit status")

    # POSIX wait status exposes the low eight bits of a normal process exit.
    expected = args.status & 0xff if os.name == "posix" else args.status
    result = subprocess.run(
        args.command,
        stdout=subprocess.PIPE if args.empty_stdout else None,
        stderr=subprocess.PIPE if args.empty_stderr else None,
    )
    if result.stdout:
        sys.exit(f"Expected empty stdout, got {result.stdout!r}")
    if result.stderr:
        sys.exit(f"Expected empty stderr, got {result.stderr!r}")
    if result.returncode != expected:
        sys.exit(f"Expected exit status {expected}, got {result.returncode}")


if __name__ == "__main__":
    main()
