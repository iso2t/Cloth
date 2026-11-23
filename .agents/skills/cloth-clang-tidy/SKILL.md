---
name: cloth-clang-tidy
description: Check a Cloth patch before review with clang-tidy, including the translation units that consume changed C++ headers. Invoke explicitly with $cloth-clang-tidy.
---

# Check a Cloth patch with clang-tidy

<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->

Run the checker from the repository root. Use report mode unless the user explicitly asks to apply fixes. [Patch checker](../../../scripts/clang_tidy_patch.py)

```console
python scripts/clang_tidy_patch.py
```

The checker compares the working tree with the merge base of `origin/HEAD` (or `HEAD` when the remote default is unavailable) and includes untracked files. For a review with a known target branch, pass it explicitly, for example `--base origin/master`. Use `--base HEAD` for just the staged, unstaged and untracked patch.

It reads the root `.clang-tidy` and looks for `compile_commands.json` at the root and then in `build/`. Pass `-p path/to/build` for another database. Otherwise the checker derives a temporary database from the native Make recipes for the compiler and unit tests. [Lint configuration](../../../.clang-tidy)

Put `clang-tidy`, `clang-scan-deps` and `clang` from the same LLVM installation on `PATH`. If dependency headers are missing, prepare them with `make test-build` or `mingw32-make test-build` on Windows. [Build guide](../../../Documentation/building/Building.rst)

For a changed `.hpp`, the checker scans direct and transitive includes under the recorded compile flags. It runs clang-tidy on each affected translation unit and reports findings in the changed files. Summarize findings with their file, line and check name. Identify any changed files the database could not cover. Report mode exits nonzero for findings or an incomplete check.

When the user asks for fixes, run the same command with `--fix`, inspect the diff and rerun in report mode to identify remaining findings.
