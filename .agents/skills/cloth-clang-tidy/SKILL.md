---
name: cloth-clang-tidy
description: Run clang-tidy on a Cloth C++ patch or the whole current project before a push. Apply and verify cleanups when asked. Invoke explicitly with $cloth-clang-tidy.
---

# Review and clean up Cloth C++

<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->

Use the repository checker to run clang-tidy, then read the affected C++ and explain or make the edits. A request to check or review gets a read-only report. A request to clean up code or apply fixes authorizes edits within the requested scope. [Clang-tidy checker](../../../scripts/clang_tidy_patch.py)

## Choose the scope

Run from the repository root. For a pre-push or all-files check, use `--all` on demand. It checks the current checkout's tracked and unignored untracked C++ sources and `.hpp` headers, excluding vendored `ThirdParty/` and generated `build/` files. It also works on a clean working tree.

The default patch command includes branch changes since the merge base with `origin/HEAD`, local edits and untracked files. It falls back to `HEAD` if the remote default is unavailable.

| Requested scope | Report command |
| --- | --- |
| Whole project before a push | `python scripts/clang_tidy_patch.py --all` |
| Current branch patch | `python scripts/clang_tidy_patch.py` |
| Local edits only | `python scripts/clang_tidy_patch.py --base HEAD` |
| Latest commit in this checkout, including any local edits | `python scripts/clang_tidy_patch.py --base HEAD^` |
| Pull request | Pass `--base` with the fetched target branch, such as `origin/master`. |

Report the source and header counts for a full-project run or the comparison base for a patch. For a patch already committed at `HEAD`, compare against its parent. If the comparison selects zero files, say how many files were checked. For a completed scan, give the number of translation units and the findings.

## Review the proposed edits

For each distinct finding, read the statement and its surrounding code. Show the file and line, the actual expression or declaration that would change, its replacement, and why the change fits. Group repeated instances, count them and show one example. Patch mode covers entire changed files, so identify older code when it matters to the review.

Follow Cloth's preference for explicit types where they aid understanding. A cast that already spells out its target type is a good candidate for `auto`. For a proposed reference in place of a copy, check the source lifetime and whether later mutation would change the value observed. [C++ style](../../../Documentation/contributing/Code%20Style.rst)

## Apply requested cleanup

When asked to apply all fixes in the selected scope, add `--fix` to the same command. The checker applies a fix only when its primary diagnostic and every replacement belong to the selected files. It reports which files it edited, then checks the translation units again. For a request limited to a specific cleanup, inspect the relevant patch hunk even if clang-tidy has no fix-it. Make the requested edit directly when you can verify its behavior, then rerun the report with the same base.

Inspect the resulting diff. Resolve a remaining diagnostic by hand when the requested change and its behavior are clear. Run the relevant build or test for edits affecting types, lifetimes or control flow. Report a representative before-and-after change, the verification result and any outstanding findings or files the database could not cover. [Test targets](../../../Tests/Makefile)

## Compilation context

The checker uses the root `.clang-tidy` and the compilation database in the repository root or `build/`. Supply another database directory with `-p`. When neither default location has one, the checker derives a temporary database from Cloth's Make recipes. Headers are checked through their direct and transitive C++ consumers under the recorded compiler flags. The report uses each finding's primary location and surfaces compilation failures that prevent analysis. [Lint configuration](../../../.clang-tidy)

Use `clang-tidy`, `clang-scan-deps` and `clang` from the same LLVM installation. If dependency headers are missing, prepare them with `make test-build` or `mingw32-make test-build` on Windows. [Build guide](../../../Documentation/building/Building.rst)

Fix mode also uses `clang-apply-replacements` and Python's PyYAML package to keep edits in scope. To install PyYAML for this checkout, run `python -m pip install --target build/clang-tidy-python PyYAML`.
