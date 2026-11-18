---
name: cloth-repository
description: Find component owners, native build commands, Shuttle's manifest status, test organization, and
reStructuredText contribution guides in the Cloth checkout.
---

# Cloth repository

<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->

## Component map

| Area          | Where to start                                                                                                                                                                                                                                                                                                                                                                                                  |
|---------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Compiler      | The C++ CLI loads a source file, runs the lexer and parser, and passes a `cloth::SourceUnit` to the backend. [Driver](../../../Compiler/src/cloth.cpp) · [Frontend internals](../cloth-frontend/SKILL.md) · [Cloth syntax](../cloth-language/SKILL.md)                                                                                                                                                          |
| Backend       | This C++ component owns semantic checking, optimization, and LLVM emission. Its current `BackendObject` constructor copies the parsed `SourceUnit`. The driver returns after constructing it. [Backend interface](../../../Backend/include/backend.hpp) · [Implementation](../../../Backend/src/backend.cpp)                                                                                                    |
| Diagnostics   | The shared C++ `cde` library defines diagnostic codes, source labels, and rendering. Start here for a diagnostic's identity or presentation. [Diagnostics skill](../cloth-diagnostics/SKILL.md)                                                                                                                                                                                                                 |
| Shuttle       | Cloth's build system is a separate Rust crate. Its entry point prints `Hello, world!`. Project manifests are moving from TOML to KDL. The crate still lists `kcl-lib`, and project-manifest parsing is pending. `Cargo.toml` describes the Rust crate itself. [Crate](../../../Shuttle/Cargo.toml) · [Entry point](../../../Shuttle/src/main.rs)                                                                |
| Documentation | Project and component overviews, build instructions, and contribution guides live in `Documentation/` as reStructuredText (`.rst`). [Welcome](../../../Documentation/Welcome.rst) · [C++ style](../../../Documentation/contributing/Code%20Style.rst) · [RFC process](../../../Documentation/contributing/RFC%20Process.rst) · [AI-assisted contributions](../../../Documentation/contributing/AI%20Agents.rst) |
| Tests         | LLVM lit runs driver, lexer, and diagnostics fixtures. GoogleTest checks lexer tokens, parser trees, and diagnostic labels through the C++ interfaces. Root Make targets select one case, one area, or the full suite. [Test targets](../../../Tests/Makefile) · [Testing guide](../../../Documentation/contributing/Testing.rst) |

## Build and exercise the native compiler

Run `mingw32-make` from the repository root on Windows with MinGW, or `make` on Linux and macOS. The root Makefile
builds `build/cloth.exe` on Windows and `build/cloth` on Unix. Each C++ component has a Kbuild object list. The compiler
flags file sets C++20 and the three include
roots. [Makefile](../../../Makefile) · [Compiler objects](../../../Compiler/Kbuild) · [Diagnostics objects](../../../Diagnostics/Kbuild) · [Backend objects](../../../Backend/Kbuild) · [Compiler flags](../../../compile_flags.txt) · [Build guide](../../../Documentation/building/Building.rst)

With `$source` set to an existing UTF-8 `.co` or `.clib` file, these PowerShell invocations exercise the parser and
print the lexer table, respectively.

~~~powershell
.\build\cloth.exe --src "$source"
.\build\cloth.exe -L --src "$source"
~~~

The same arguments work with `./build/cloth` on Unix. With only `--src` and a path, the driver returns `0` after the
backend handoff. The `-l` mode scans silently and returns `0xD00001`. The `-L` mode writes the table to stdout and
returns `0xD00002`. These are the driver's return values. The Makefile's `run` recipe supplies no source
argument. [CLI flags](../../../Documentation/compiler/Flags.rst) · [Return constants](../../../Compiler/include/errors/cloth_errors.hpp)
