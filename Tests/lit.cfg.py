# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from pathlib import Path
import platform
import shlex
import sys

import lit.formats


def tool_path(path):
    return shlex.quote(Path(path).as_posix())


suite = Path(__file__).resolve().parent
config.name = "Cloth"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".co", ".clib", ".test"]
config.excludes = ["Inputs", "Unit", "Support"]
config.test_source_root = str(suite)

params = lit_config.params
if not all(key in params for key in ("exec_root", "cloth", "filecheck", "runtime")):
    lit_config.fatal("Run the suite from the repository root with 'make test' or 'mingw32-make test'.")
if "case" in params:
    case = Path(params["case"])
    if any(part in config.excludes for part in case.parts):
        lit_config.fatal(f"{case} is test support data. Select the fixture containing the RUN lines.")
    if case.name.startswith(".") or case.suffix not in config.suffixes:
        lit_config.fatal(f"Select a .co, .clib, or .test fixture. Received {case}.")

config.test_exec_root = params["exec_root"]
config.available_features.add(sys.platform)
if platform.machine().lower() in {"amd64", "x86_64", "x86", "i386", "i486", "i586", "i686"}:
    config.available_features.add("x86")
config.substitutions.extend([
    ("%python", tool_path(sys.executable)),
    ("%cloth", tool_path(params["cloth"])),
    ("%migrate_semicolons", tool_path(Path(params["cloth"]).with_name("cloth-migrate-semicolons" + Path(params["cloth"]).suffix))),
    ("%filecheck", tool_path(params["filecheck"])),
    ("%llvm_readobj", tool_path(params.get("readobj", "llvm-readobj"))),
    ("%llvm_mc", tool_path(params.get("llvm_mc", "llvm-mc"))),
    ("%cc", params.get("cc", "gcc" if sys.platform == "win32" else "cc")),
    ("%stdlib", tool_path(suite.parent / "Stdlib/StandardLibrary/src")),
    ("%runtime", "-I " + tool_path(suite.parent / "Runtime/include") + " " + tool_path(params["runtime"]) + (" -lshell32" if sys.platform == "win32" else "")),
    ("%host_os", sys.platform),
    ("%expect_exit", tool_path(sys.executable) + " " + tool_path(suite / "Support" / "expect_exit.py")),
])
