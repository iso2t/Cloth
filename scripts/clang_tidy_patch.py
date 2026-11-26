# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Run clang-tidy on Cloth's C++ code and C runtime."""

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


CPP_SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".C")
SOURCE_SUFFIXES = (*CPP_SOURCE_SUFFIXES, ".c")
HEADER_SUFFIXES = (".hpp", ".h")
PROJECT_PATTERNS = [*(f"*{suffix}" for suffix in (*CPP_SOURCE_SUFFIXES, ".hpp")),
                    "Runtime/*.c", "Runtime/*.h", ":(top,exclude)ThirdParty/**", ":(top,exclude)build/**"]
DIAGNOSTIC = re.compile(rb"^(?:(.+?):\d+:\d+: )?(warning|error|fatal error|remark): ")


def git_paths(root, listing):
    return {root / os.fsdecode(name) for name in listing.split(b"\0") if name}


def project_files(root):
    listing = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *PROJECT_PATTERNS], cwd=root,
    )
    members = git_paths(root, listing)
    # A symlink's target is eligible when it is itself a project file.
    return {path.resolve() for path in members if path.is_file()} & members


def changed_files(root, base):
    if base is None:
        origin_head = subprocess.run(
            ["git", "symbolic-ref", "--quiet", "--short", "refs/remotes/origin/HEAD"],
            cwd=root, stdout=subprocess.PIPE, text=True,
        )
        base = origin_head.stdout.strip() or "HEAD"
    ancestor = subprocess.check_output(["git", "merge-base", "HEAD", base], cwd=root, text=True).strip()
    print(f"Checking the working tree against {base} ({ancestor[:12]}).", flush=True)
    tracked = subprocess.check_output(
        ["git", "diff", "--no-renames", "--name-only", "-z", ancestor, "--", *PROJECT_PATTERNS], cwd=root,
    )
    untracked = subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard", "-z", "--", *PROJECT_PATTERNS], cwd=root,
    )
    return git_paths(root, tracked + untracked)


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


def set_windows_gnu_target(entry, driver_targets):
    # Windows LLVM otherwise interprets a plain gcc command as targeting the MSVC CRT.
    arguments = entry.get("arguments")
    invocation = quote_command(arguments) if arguments else entry["command"]
    driver = arguments[0] if arguments else re.match(r'^\s*("[^"]+"|\S+)', invocation)[1].strip('"')
    if Path(driver).stem.lower() not in {"gcc", "g++"} or re.search(r"(?:^|\s)--?target[=\s]", invocation):
        return
    if driver not in driver_targets:
        executable = shutil.which(driver)
        if not executable:
            return
        driver_targets[driver] = subprocess.check_output([executable, "-dumpmachine"], text=True).strip()
    flag = f"--target={driver_targets[driver]}"
    if arguments:
        entry["arguments"] = [*arguments, flag]
    else:
        entry["command"] += " " + flag


def project_commands(database, members):
    entries, driver_targets = [], {}
    for entry in json.loads(database.read_text(encoding="utf-8-sig")):
        directory = (database.parent / entry["directory"]).resolve()
        source = (directory / entry["file"]).resolve()
        if source.suffix in SOURCE_SUFFIXES and source in members:
            entry = dict(entry, file=str(source), directory=str(directory))
            if os.name == "nt":
                set_windows_gnu_target(entry, driver_targets)
            entries.append(entry)
    return entries


def header_consumers(entries, headers, work, tidy):
    if not entries:
        return set(), set()
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
    header_names = {os.path.normcase(header.name) for header in headers}
    for unit in json.loads(output)["translation-units"]:
        for command in unit["commands"]:
            dependencies = (Path(name).resolve() for name in command["file-deps"]
                            if os.path.normcase(os.path.basename(name)) in header_names)
            included = headers.intersection(dependencies)
            if included:
                covered.update(included)
                consumers.add(Path(command["input-file"]))
    return consumers, covered


def run_tidy(tidy, options, root, work, files):
    # A shared header can select enough files to exceed a Windows command line.
    response = work / "tidy.rsp"
    options = ["--quiet", "--use-color=false", "--warnings-as-errors=-*", *options]
    response.write_text(quote_command(options), encoding="utf-8")
    result = subprocess.run([tidy, f"@{response}"], cwd=root, stdout=subprocess.PIPE)

    # LLVM's line filter accepts a warning when *any note* matches. The
    # overlay gives every selected file an absolute name in the diagnostic.
    # Keep notes with their primary message, and always surface compiler errors.
    show = True
    findings = 0
    for line in result.stdout.splitlines(keepends=True):
        match = DIAGNOSTIC.match(line)
        if match:
            path, level = match.groups()
            show = not path or Path(os.fsdecode(path)) in files or level in (b"error", b"fatal error")
            if show and level != b"remark":
                findings += 1
        if show:
            sys.stdout.buffer.write(line)
    sys.stdout.buffer.flush()
    return result.returncode, findings


def apply_selected_fixes(export, before, root, work, tidy):
    if not export.is_file():
        return
    sys.path.append(str(root / "build" / "clang-tidy-python"))
    try:
        import yaml
    except ModuleNotFoundError as error:
        raise ValueError("To apply fixes, install PyYAML for this checkout with "
                         "python -m pip install --target build/clang-tidy-python PyYAML") from error

    replacements = {}
    withheld = 0
    # C++ replacement text such as "NO" must stay a string even when a YAML
    # reader would otherwise interpret the scalar as a boolean or a number.
    loader = getattr(yaml, "CBaseLoader", yaml.BaseLoader)
    for diagnostic in yaml.load(export.read_bytes(), Loader=loader)["Diagnostics"]:
        directory = Path(diagnostic.get("BuildDirectory") or root)
        message = diagnostic["DiagnosticMessage"]
        if (directory / message.get("FilePath", "")).resolve() not in before:
            continue
        edits = message.get("Replacements", [])
        paths = [(directory / edit["FilePath"]).resolve() for edit in edits]
        # A rename or signature change can edit several files. Keep the fix
        # together so an out-of-scope reference cannot be partially rewritten.
        if any(path not in before for path in paths):
            withheld += 1
            continue
        for edit, path in zip(edits, paths):
            offset, length = int(edit["Offset"]), int(edit["Length"])
            key = (path, offset, length, edit["ReplacementText"])
            replacements[key] = dict(edit, FilePath=str(path), Offset=offset, Length=length)
    if withheld:
        print(f"Left {withheld} fix(es) for review because they also edit files outside the selection.", flush=True)
    if replacements:
        for path in {key[0] for key in replacements}:
            if path.read_bytes() != before[path]:
                raise ValueError(f"{path} changed during analysis. Rerun the checker to refresh its fixes.")
        approved = work / "selected-fixes"
        approved.mkdir()
        document = {"MainSourceFile": "", "Replacements": list(replacements.values())}
        (approved / "fixes.yaml").write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
        apply = llvm_tool("clang-apply-replacements", tidy.resolve().parent)
        config = subprocess.check_output([tidy, "--dump-config", f"--config-file={root / '.clang-tidy'}"], cwd=root)
        style = yaml.safe_load(config)["FormatStyle"]
        formatting = ["--format", f"--style-config={root}"] if style.lower() != "none" else []
        subprocess.run([apply, "--ignore-insert-conflict", f"--style={style}", *formatting, approved], cwd=root, check=True)


def check_files(args, root, work):
    project = project_files(root)
    targets = project if args.all_files else changed_files(root, args.base)
    sources = {path for path in targets if path.suffix in SOURCE_SUFFIXES and path.is_file()}
    headers = {path for path in targets if path.suffix in HEADER_SUFFIXES}
    if args.all_files:
        print(f"Checking the project in {root} ({len(sources)} sources and {len(headers)} headers).", flush=True)
    if not sources and not headers:
        print("clang-tidy checked 0 files.")
        if not args.all_files:
            print("To include committed files, use --all for this project or --base HEAD^ for the latest commit.")
        return 0

    tidy = llvm_tool("clang-tidy")
    database = compilation_database(root, args.build_dir, work)
    entries = project_commands(database, project | sources)
    (work / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
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
    # clang-tidy canonical names for the selected files so its suffix-based line filter
    # matches the correct files even when two headers have the same basename.
    files = {path for path in targets if path.is_file()}
    names = sorted(str(path) for path in files)
    overlay = work / "paths.json"
    overlay.write_text(json.dumps({"version": 0, "roots": [
        {"type": "file", "name": name, "external-contents": name} for name in names
    ]}), encoding="utf-8")
    line_filter = json.dumps([{"name": name} for name in names])
    options = [
        "-p", str(work), f"--config-file={root / '.clang-tidy'}",
        f"--vfsoverlay={overlay}", f"--line-filter={line_filter}",
        *(str(path.relative_to(root)) for path in sorted(selected)),
    ]
    if args.fix:
        before = {path: path.read_bytes() for path in files}
        print(f"Collecting clang-tidy fixes across {len(selected)} translation unit(s).", flush=True)
        export = work / "diagnostics.yaml"
        status, findings = run_tidy(tidy, [f"--export-fixes={export}", *options], root, work, files)
        if status:
            return 2
        if findings:
            apply_selected_fixes(export, before, root, work, tidy)
        edited = sorted(path for path in files if path.read_bytes() != before[path])
        print(f"clang-tidy edited {len(edited)} file(s).", flush=True)
        for path in edited:
            print(f"  {path.relative_to(root)}", flush=True)

    print(f"Checking {len(selected)} translation unit(s) for findings.", flush=True)
    status, findings = run_tidy(tidy, options, root, work, files)
    if status or missing:
        return 2
    if findings:
        print(f"{findings} clang-tidy finding(s) in the selected files.")
        return 1
    print("No clang-tidy findings in the selected files.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--all", dest="all_files", action="store_true",
                       help="check all project C++ files and runtime C files, including untracked files")
    scope.add_argument("--base", help="compare from the merge base with this ref (default: origin/HEAD, then HEAD)")
    parser.add_argument("-p", "--build-dir", type=Path, help="directory containing compile_commands.json")
    parser.add_argument("--fix", action="store_true", help="apply suggested fixes and check for remaining findings")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        with tempfile.TemporaryDirectory(prefix="cloth-tidy-") as directory:
            return check_files(args, root, Path(directory))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"clang-tidy check failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
