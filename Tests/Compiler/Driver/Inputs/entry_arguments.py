# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import os
import subprocess
import sys


def check(executable, args, status, stdout=b"", stderr=b""):
    result = subprocess.run([executable, *args], capture_output=True, timeout=15)
    actual = (result.returncode, result.stdout, result.stderr)
    assert actual == (status, stdout, stderr), (args, actual)


def main():
    executable, mode = sys.argv[1:]
    message = 'quoted "word" \\ \u96ea\U0001f600'
    if mode == "status":
        check(executable, [], 0xfffffffe if os.name == "nt" else 254)
        return
    check(executable, [], 42)
    if mode == "nullable":
        check(executable, [""], 7)
        check(executable, [message], 7, message.encode("utf-8"))
    else:
        check(executable, ["", "a b", message], 7, message.encode("utf-8"))
        if os.name != "nt":
            for invalid in (b"\xc0\xaf", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xf0\x9f"):
                check(executable, [invalid], 1, stderr=b"Cloth runtime error: invalid Unicode in process arguments\n")


if __name__ == "__main__":
    main()
