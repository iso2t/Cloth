# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


with TemporaryDirectory() as folder:
    root = Path(folder)
    # Some filesystems can store names that are not Unicode scalar sequences.
    unrelated = "ignored\ud800" if os.name == "nt" else os.fsdecode(b"ignored\xff")
    try:
        (root / unrelated).touch()
    except (OSError, UnicodeError):
        pass

    (root / "Cafe\u0301.co").write_text("pub fn ok() {}\n", encoding="utf-8")
    entry = root / "Main.co"
    entry.write_text("using Caf\u00e9: ok;\nfn check() { ok(); }\n", encoding="utf-8")
    command = [sys.argv[1], "--src", str(entry)]
    subprocess.run(command, check=True)

    # Normalization-sensitive filesystems can hold two distinct candidates.
    composed = root / "Caf\u00e9.co"
    if not composed.exists():
        composed.write_text("pub fn ok() {}\n", encoding="utf-8")
        result = subprocess.run(command, capture_output=True)
        if result.returncode != 1 or b"[C3099]" not in result.stderr:
            raise AssertionError(f"Expected an ambiguous import, got {result.returncode}: {result.stderr!r}")

    (root / "Mixed.CO").write_text("pub fn ok() {}\n", encoding="utf-8")
    if (root / "Mixed.co").is_file():
        entry.write_text("using Mixed: ok;\nfn check() { ok(); }\n", encoding="utf-8")
        subprocess.run(command, check=True)
