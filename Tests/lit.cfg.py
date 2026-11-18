# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from pathlib import Path
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
if not all(key in params for key in ("exec_root", "cloth", "filecheck")):
    lit_config.fatal("Run the suite from the repository root with 'make test' or 'mingw32-make test'.")
if "case" in params:
    case = Path(params["case"])
    if any(part in config.excludes for part in case.parts):
        lit_config.fatal(f"{case} is test support data. Select the fixture containing the RUN lines.")
    if case.name.startswith(".") or case.suffix not in config.suffixes:
        lit_config.fatal(f"Select a .co, .clib, or .test fixture. Received {case}.")

config.test_exec_root = params["exec_root"]
config.substitutions.extend([
    ("%cloth", tool_path(params["cloth"])),
    ("%filecheck", tool_path(params["filecheck"])),
    ("%expect_exit", tool_path(sys.executable) + " " + tool_path(suite / "Support" / "expect_exit.py")),
])
