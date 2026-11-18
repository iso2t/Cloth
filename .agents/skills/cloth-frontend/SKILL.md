---
name: cloth-frontend
description: Trace Cloth source through the C++ loader, token stream, parser, and AST. Use for lexer or parser changes
and questions about how accepted syntax is represented.
---

# Cloth C++ frontend

<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->

Start at `Compiler/src/cloth.cpp` for CLI behavior. On the ordinary `--src` path, `SourceFile` validates the extension
and UTF-8 bytes. `Parser(Lexer&)` calls `scanTokens()`, then `Parser::parse()` builds the `SourceUnit` passed to
`backend::BackendObject`. A source unit records declarations, a separate list of header imports, and the end-of-file
span. [Driver](../../../Compiler/src/cloth.cpp) · [File loader](../../../Compiler/src/file/source_file.cpp) · [Parser constructors](../../../Compiler/include/parser/parser.hpp) · [Source unit](../../../Compiler/include/ast/source_unit.hpp)

## Tokens and source locations

- Find a keyword or operator's spelling in `token.cpp` and its enum in `token.hpp`. The lexer builds its operator table
  from entries marked tokenized and matches the longest spelling first. For example, `..=` survives as one token. The
  stand-alone `_` is an operator while `_name` is an
  identifier. [Spelling tables](../../../Compiler/src/token/token.cpp) · [Token model](../../../Compiler/include/token/token.hpp) · [Scanner](../../../Compiler/src/lexer/lexer.cpp)
- For a number, follow `Lexer::number` for token boundaries and separator errors, then the numeric literal helpers for
  the value or spelling stored by the expression parser. Interpolated strings emit alternating segment and hole tokens
  that the expression parser
  combines. [Literal decoding](../../../Compiler/src/token/numeric_literals.cpp) · [Expression parser](../../../Compiler/src/parser/subs/expression_parser.cpp)
- `TokenSpan` uses one-based lines and UTF-8 byte columns with an exclusive end. Spans carry a shared source file.
  `span_through` retains the source when both endpoints refer to the same file. Carry the original spans into AST nodes
  and
  diagnostics. [TokenSpan](../../../Compiler/include/token/token.hpp) · [Diagnostic labels](../cloth-diagnostics/SKILL.md)

## Finding the grammar rule

| Question                                                                   | Owner                                                                                                                                                                                                                                                                                                                                                                                    |
|----------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Where may a declaration, import, assignment, or statement separator occur? | `Parser::parseDeclarations` and `parseStatements` own the surrounding grammar. `ImportParser` handles the header's path and selection. [Parser](../../../Compiler/src/parser/parser.cpp) · [Imports](../../../Compiler/src/parser/subs/import_parser.cpp)                                                                                                                                |
| How are types, functions, or members spelled?                              | The subparsers share the parent parser's token cursor. Follow the owning declaration parser and its corresponding `ast/decl` header. `TypeParser` handles type suffixes and generic lookahead. [Subparsers](../../../Compiler/src/parser/subs/) · [Declaration nodes](../../../Compiler/include/ast/decl/) · [Type parser](../../../Compiler/src/parser/subs/type_parser.cpp)            |
| How does an expression group?                                              | `ExpressionParser` handles primary, postfix, and infix forms. Assignment and postfix update statements return to `Parser::parseStatement`. Range loop headers consume their own outer range delimiter. [Expressions](../../../Compiler/src/parser/subs/expression_parser.cpp) · [For loops](../../../Compiler/src/parser/subs/loops/for_loop_parser.cpp)                                 |
| Who consumes the closing delimiter or newline?                             | `DelimitedParser` pairs delimiters and reads comma-separated items. The enclosing grammar controls which newlines it skips. Block comments can cross lines without emitting a newline token, so imports also compare token spans. [Delimiter helpers](../../../Compiler/include/parser/subs/delimited_parser.hpp) · [Import parser](../../../Compiler/src/parser/subs/import_parser.cpp) |

Use `-L --src` to inspect lexemes, token kinds, and spans. Use `--src` with a complete file to check parser acceptance.
For questions about AST shape, trace the constructor at the accepting
branch. [Token-table printer](../../../Compiler/src/util/pstream.cpp) · [AST headers](../../../Compiler/include/ast/) · [Build and invocation](../cloth-repository/SKILL.md)
