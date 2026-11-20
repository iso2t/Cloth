---
name: cloth-repository
description: Find Cloth component owners, native build and test commands, the Shuttle CLI, and contribution guides.
---

# Cloth repository

<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->

## Component map

| Area | Where to start |
|------|----------------|
| Compiler | The native driver parses a `SourceUnit`, builds a `SymbolTable`, and constructs a `CheckedProgram`. The frontend resolves signatures across imports before checking bodies, overloads, and control flow. [Driver](../../../Compiler/src/cloth.cpp) · [Frontend internals](../cloth-frontend/SKILL.md) · [Cloth syntax](../cloth-language/SKILL.md) |
| Backend | `backend::BackendObject` takes ownership of the frontend's `CheckedProgram`. Its constructor stores the program and the driver returns. [Backend interface](../../../Backend/include/backend/backend.hpp) · [Implementation](../../../Backend/src/backend/backend.cpp) |
| Diagnostics | The shared C++ `cde` library defines diagnostic codes, source labels, and rendering. [Diagnostics skill](../cloth-diagnostics/SKILL.md) |
| Shuttle | The Rust CLI reports that object emission is unavailable and directs `shuttle build` callers to the native frontend check. [Crate](../../../Shuttle/Cargo.toml) · [Entry point](../../../Shuttle/src/main.rs) |
| Documentation | Project and component overviews, build instructions, and contribution guides live in `Documentation/` as reStructuredText. [Welcome](../../../Documentation/Welcome.rst) · [C++ style](../../../Documentation/contributing/Code%20Style.rst) · [RFC process](../../../Documentation/contributing/RFC%20Process.rst) · [AI-assisted contributions](../../../Documentation/contributing/AI%20Agents.rst) |
| Tests | LLVM lit runs driver and source-file fixtures. GoogleTest checks the lexer, parser, semantic checker, and diagnostics through the C++ interfaces. [Test targets](../../../Tests/Makefile) · [Testing guide](../../../Documentation/contributing/Testing.rst) |

## Build and exercise the native compiler

Run `mingw32-make` from the repository root on Windows with MinGW, or `make` on Linux and macOS. The root Makefile
builds `build/cloth.exe` on Windows and `build/cloth` on Unix. Each C++ component has a Kbuild object list. The compiler
flags file sets C++20 and the include paths. [Makefile](../../../Makefile) · [Compiler objects](../../../Compiler/Kbuild) · [Diagnostics objects](../../../Diagnostics/Kbuild) · [Backend objects](../../../Backend/Kbuild) · [Compiler flags](../../../compile_flags.txt) · [Build guide](../../../Documentation/building/Building.rst)

Set `$source` to a UTF-8 `.co` or `.clib` file. The first command checks the file and its imports. The second prints
the lexer table.

~~~powershell
.\build\cloth.exe --src "$source"
.\build\cloth.exe -L --src "$source"
~~~

The same arguments work with `./build/cloth` on Unix. After a successful frontend check and backend handoff, the driver returns `0`. The `-l` mode scans silently and returns `0xD00001`. The `-L` mode writes the table to stdout and
returns `0xD00002`. These are the driver's return values. The Makefile's `run` target checks `Stdlib/cloth/Library.co` and its imports in library mode. [CLI flags](../../../Documentation/compiler/Flags.rst) · [Return constants](../../../Compiler/include/errors/cloth_errors.hpp)
