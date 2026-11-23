# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Run clang-tidy on the C++ files in a Cloth patch."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


def changed_files(root, base):
    if base is None:
        origin_head = subprocess.run(
            ["git", "symbolic-ref", "--quiet", "--short", "refs/remotes/origin/HEAD"],
            cwd=root, stdout=subprocess.PIPE, text=True,
        )
        base = origin_head.stdout.strip() or "HEAD"
    ancestor = subprocess.check_output(["git", "merge-base", "HEAD", base], cwd=root, text=True).strip()
    print(f"Checking the working tree against {base} ({ancestor[:12]}).", flush=True)
    patterns = ["--", "*.cpp", "*.hpp"]
    tracked = subprocess.check_output(
        ["git", "diff", "--no-renames", "--name-only", "-z", ancestor, *patterns], cwd=root,
    )
    untracked = subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard", "-z", *patterns], cwd=root,
    )
    return {root / os.fsdecode(name) for name in (tracked + untracked).split(b"\0") if name}


def compilation_database(root, build_dir, work):
    if build_dir is not None:
        database = build_dir.resolve() / "compile_commands.json"
        if not database.is_file():
            raise ValueError(f"Missing compilation database at {database}")
        return database
    for directory in (root, root / "build"):
        database = directory / "compile_commands.json"
        if database.is_file():
            return database

    make = "mingw32-make" if os.name == "nt" else "make"
    print(f"Reading compile commands from {make} -Bn test-build.", flush=True)
    recipes = subprocess.check_output([make, "-Bn", "test-build"], cwd=root, text=True)
    entries = []
    # Cloth's object recipes all end in -c "$<" -o "$@". Keep the command
    # verbatim so LLVM interprets the response file, defines and quoting.
    for command in recipes.splitlines():
        match = re.search(r' -c "([^"]+)" -o "[^"]+"$', command)
        if match:
            entries.append({"directory": str(root), "file": match[1], "command": command})
    if not entries:
        raise ValueError("Make produced no compile commands. Supply a compilation database with -p.")
    database = work / "compile_commands.json"
    database.write_text(json.dumps(entries), encoding="utf-8")
    return database


def llvm_tool(name, directory=None):
    path = shutil.which(name, path=str(directory)) if directory else None
    path = path or shutil.which(name)
    if not path:
        raise ValueError(f"Could not find {name}. Add the LLVM tools directory to PATH.")
    return Path(path).absolute()


def quote_command(arguments):
    return subprocess.list2cmdline(arguments) if os.name == "nt" else shlex.join(arguments)


def cpp_commands(database, root):
    entries = []
    for entry in json.loads(database.read_text(encoding="utf-8-sig")):
        directory = (database.parent / entry["directory"]).resolve()
        source = (directory / entry["file"]).resolve()
        if source.suffix in {".cpp", ".cc", ".cxx", ".C"} and source.is_relative_to(root) and source.is_file():
            entries.append(dict(entry, file=str(source), directory=str(directory)))
    return entries


def header_consumers(entries, headers, work, tidy):
    scanner = llvm_tool("clang-scan-deps", tidy.resolve().parent)
    clang = llvm_tool("clang", scanner.resolve().parent)
    resource_dir = subprocess.check_output([clang, "-print-resource-dir"], text=True).strip()
    # A GCC compilation database points the scanner at GCC's installation.
    # Supply Clang's builtin headers for its preprocessor in that case.
    flag = f"-resource-dir={resource_dir}"
    for entry in entries:
        if "arguments" in entry:
            if not any("-resource-dir" in arg for arg in entry["arguments"]):
                entry["arguments"] = [*entry["arguments"], flag]
        elif "-resource-dir" not in entry["command"]:
            entry["command"] += " " + quote_command([flag])
    scan_db = work / "scan.json"
    scan_db.write_text(json.dumps(entries), encoding="utf-8")
    output = subprocess.check_output([scanner, "-format=experimental-full", "-compilation-database", scan_db])
    covered = set()
    consumers = set()
    for unit in json.loads(output)["translation-units"]:
        for command in unit["commands"]:
            dependencies = (Path(name).resolve() for name in command["file-deps"] if name.endswith(".hpp"))
            included = headers.intersection(dependencies)
            if included:
                covered.update(included)
                consumers.add(Path(command["input-file"]))
    return consumers, covered


def check_patch(args, root, work):
    changed = changed_files(root, args.base)
    sources = {path for path in changed if path.suffix == ".cpp" and path.is_file()}
    headers = {path for path in changed if path.suffix == ".hpp"}
    if not sources and not headers:
        print("No changed C++ sources or headers.")
        return 0

    tidy = llvm_tool("clang-tidy")
    database = compilation_database(root, args.build_dir, work)
    entries = cpp_commands(database, root)
    selected = sources.intersection(Path(entry["file"]) for entry in entries)
    missing = sources - selected
    for path in sorted(missing):
        print(f"No compile command for {path.relative_to(root)}.", file=sys.stderr)
    if headers:
        consumers, covered = header_consumers(entries, headers, work, tidy)
        selected.update(consumers)
        # A deleted header has no current owner. The scan still catches a
        # dangling include, even when the header was the only changed file.
        for path in sorted(headers - covered):
            if path.is_file():
                print(f"No translation unit in the compilation database includes {path.relative_to(root)}.", file=sys.stderr)
                missing.add(path)
    if not selected:
        print("No translation units to lint.")
        return 2 if missing else 0

    # FileEntry names can be relative, or mix slash styles on Windows. Give
    # clang-tidy canonical names for the patch so its suffix-based line filter
    # matches the correct files even when two headers have the same basename.
    files = sorted(str(path) for path in changed if path.is_file())
    overlay = work / "paths.json"
    overlay.write_text(json.dumps({"version": 0, "roots": [
        {"type": "file", "name": name, "external-contents": name} for name in files
    ]}), encoding="utf-8")
    line_filter = json.dumps([{"name": name} for name in files])
    options = [
        "-p", str(database.parent), f"--config-file={root / '.clang-tidy'}",
        f"--vfsoverlay={overlay}", f"--line-filter={line_filter}",
        "--fix" if args.fix else "--warnings-as-errors=*",
        *(str(path.relative_to(root)) for path in sorted(selected)),
    ]
    # A shared header can select enough files to exceed a Windows command line.
    response = work / "tidy.rsp"
    response.write_text(quote_command(options), encoding="utf-8")
    print(f"Running clang-tidy on {len(selected)} translation unit(s).", flush=True)
    result = subprocess.run([tidy, f"@{response}"], cwd=root)
    return 2 if missing else result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="compare from the merge base with this ref (default: origin/HEAD, then HEAD)")
    parser.add_argument("-p", "--build-dir", type=Path, help="directory containing compile_commands.json")
    parser.add_argument("--fix", action="store_true", help="apply clang-tidy's suggested fixes")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        with tempfile.TemporaryDirectory(prefix="cloth-tidy-") as directory:
            return check_patch(args, root, Path(directory))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"Patch check failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
