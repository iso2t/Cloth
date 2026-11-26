---
name: cloth-diagnostics
description: Trace Cloth frontend errors from their C++ throw sites through stable CDE codes, source labels, and the Musubi diagnostic reporter.
---

# Cloth diagnostics

<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->

Start at the throw site for the failing input. The driver catches `cde::ClothDiagnostic`, reports it on stderr, and returns `1`. It also catches `std::exception` and prints `what()` directly. The lexer raises `cde::SyntaxException` with the source file and UTF-8 byte span for malformed tokens and literals. [Driver](../../../Compiler/src/cloth.cpp) · [Lexer](../../../Compiler/src/lexer/lexer.cpp) · [Diagnostic code registry](../../../Diagnostics/include/cde/diagnostic_code.hpp)

## Codes and labels

- `DiagnosticCode` preserves numeric identities, including retired slots. `to_string` renders a code such as `C2010`. Follow the syntax, import, or generic error factory to see which tokens it labels and what guidance it supplies. [Code formatting](../../../Diagnostics/src/cde/diagnostic_code.cpp) · [Syntax errors](../../../Diagnostics/src/cde/syntax_errors.cpp) · [Import errors](../../../Diagnostics/src/cde/import_errors.cpp) · [Generic errors](../../../Diagnostics/src/cde/generic_errors.cpp)
- `ClothDiagnostic` carries severity, message, ordered labels, and optional guidance. `SyntaxException` inserts the primary span before related spans. Label positions are one-based UTF-8 byte columns with an exclusive end. Retain the token's source so the reporter can render the excerpt. [Diagnostic model](../../../Diagnostics/include/cde/cloth_diagnostic.hpp) · [Syntax exception](../../../Diagnostics/src/cde/syntax_exception.cpp) · [Token spans](../../../Compiler/include/token/token.hpp)
- `DiagnosticReporter` maps source spans to byte offsets, renders with the vendored Musubi library, and has a plain-text fallback if rendering fails. Terminal capabilities affect color and glyphs. [Reporter](../../../Diagnostics/src/cde/diagnostic_reporter.cpp) · [Musubi](../../../ThirdParty/musubi/)

For a two-label check, pass a file containing `using shapes: Point, Point;` to `cloth --src`. The parser reports `C2010` on the repeated `Point` and identifies the first selection in a related label. Check stderr and the exit status. Adding `-L` stops after tokenization, before import validation. [Import parser](../../../Compiler/src/parser/subs/import_parser.cpp) · [Build and invocation](../cloth-repository/SKILL.md)
