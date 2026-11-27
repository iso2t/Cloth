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
| Backend | For native output, `cloth::emitFile` calls `backend::emit` with the `CheckedProgram`. The backend lowers the program to LLVM IR, verifies the module, and returns object or assembly bytes for `emitFile` to write. [Output writer](../../../Compiler/src/file/output_file.cpp) · [Backend interface](../../../Backend/include/backend/backend.hpp) · [LLVM lowering and emission](../../../Backend/src/backend/backend.cpp) |
| Diagnostics | The shared C++ `cde` library defines diagnostic codes, source labels, and rendering. [Diagnostics skill](../cloth-diagnostics/SKILL.md) |
| Shuttle | The Rust CLI reads KDL projects from `build.k` and included modules from `module.k`. It builds a selected executable or library with the native compiler. [Manifest guide](../../../Shuttle/README.rst) · [Crate](../../../Shuttle/Cargo.toml) · [Entry point](../../../Shuttle/src/main.rs) |
| Documentation | Project and component overviews, build instructions, and contribution guides live in `Documentation/` as reStructuredText. [Welcome](../../../Documentation/Welcome.rst) · [C++ style](../../../Documentation/contributing/Code%20Style.rst) · [RFC process](../../../Documentation/contributing/RFC%20Process.rst) · [AI-assisted contributions](../../../Documentation/contributing/AI%20Agents.rst) |
| Tests | LLVM lit runs driver and source-file fixtures. GoogleTest checks the lexer, parser, semantic checker, and diagnostics through the C++ interfaces. [Test targets](../../../Tests/Makefile) · [Testing guide](../../../Documentation/contributing/Testing.rst) |

## Build and exercise the native compiler

Run `mingw32-make` from the repository root on Windows with MinGW, or `make` on Linux and macOS. The root Makefile
builds `build/cloth.exe` on Windows and `build/cloth` on Unix. Each C++ component has a Kbuild object list. The compiler
flags file sets C++20 and the include paths. [Makefile](../../../Makefile) · [Compiler objects](../../../Compiler/Kbuild) · [Diagnostics objects](../../../Diagnostics/Kbuild) · [Backend objects](../../../Backend/Kbuild) · [Compiler flags](../../../compile_flags.txt) · [Build guide](../../../Documentation/building/Building.rst)

Plain `--src` checks a UTF-8 `.co` or `.clib` file and its imports. From the repository root, check the `println`
example, then write its relocatable object and assembly listing under `build/`. All three commands return `0` on success. [Example source](../../../Tests/Compiler/Driver/Inputs/println.co) · [Native output guide](../../../Documentation/compiler/driver/Objects.rst)

~~~powershell
.\build\cloth.exe --src Tests/Compiler/Driver/Inputs/println.co
.\build\cloth.exe --kind executable --src Tests/Compiler/Driver/Inputs/println.co --emit object -o build/guide-println.o
.\build\cloth.exe --kind executable --src Tests/Compiler/Driver/Inputs/println.co --emit assembly -o build/guide-println.s
~~~

On Unix, use `./build/cloth` with the same arguments. For native library output, use `--kind library --module-name NAME`.

`-l --src PATH` scans silently and returns `0xD00001`. `-L --src PATH` prints the token table and returns `0xD00002`.
Unix shells report these statuses as `1` and `2`, respectively. The Makefile's `run` target checks
`Stdlib/StandardLibrary/src/cloth/Library.co` and its imports in library mode. [CLI flags](../../../Documentation/compiler/Flags.rst) · [Return constants](../../../Compiler/include/errors/cloth_errors.hpp)
