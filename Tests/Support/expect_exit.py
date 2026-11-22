# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
import os
import signal
import subprocess
import sys


def illegal_instruction_status():
    # The child inherits these settings when a test deliberately triggers a trap.
    if os.name == "nt":
        import ctypes

        kernel = ctypes.windll.kernel32
        kernel.SetErrorMode(kernel.GetErrorMode() | 0x0002)  # SEM_NOGPFAULTERRORBOX
        return 0xc000001d  # STATUS_ILLEGAL_INSTRUCTION
    import resource

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    return -signal.SIGILL


def expected_output(empty, lines):
    if empty:
        return b""
    if lines:
        return "".join(line + os.linesep for line in lines).encode("utf-8")
    return None


def main():
    parser = argparse.ArgumentParser(description="Check a native exit status and optional output expectations.")
    for stream in ("stdout", "stderr"):
        group = parser.add_mutually_exclusive_group()
        group.add_argument(f"--empty-{stream}", action="store_true")
        group.add_argument(f"--{stream}-line", action="append", metavar="TEXT", help=f"Expect one complete {stream} line per occurrence, in order.")
    parser.add_argument(
        "status", type=lambda value: value if value == "SIGILL" else int(value, 0),
        help="Integer exit status or SIGILL for an illegal instruction",
    )
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command:
        parser.error("expected a command after the exit status")

    if args.status == "SIGILL":
        expected = illegal_instruction_status()
    else:
        # POSIX wait status exposes the low eight bits of a normal process exit.
        expected = args.status & 0xff if os.name == "posix" else args.status
    expected_stdout = expected_output(args.empty_stdout, args.stdout_line)
    expected_stderr = expected_output(args.empty_stderr, args.stderr_line)
    result = subprocess.run(
        args.command,
        stdout=subprocess.PIPE if expected_stdout is not None else None,
        stderr=subprocess.PIPE if expected_stderr is not None else None,
    )
    if expected_stdout is not None and result.stdout != expected_stdout:
        sys.exit(f"Expected stdout {expected_stdout!r}, got {result.stdout!r}")
    if expected_stderr is not None and result.stderr != expected_stderr:
        sys.exit(f"Expected stderr {expected_stderr!r}, got {result.stderr!r}")
    if result.returncode != expected:
        sys.exit(f"Expected exit status {expected}, got {result.returncode}")


if __name__ == "__main__":
    main()
