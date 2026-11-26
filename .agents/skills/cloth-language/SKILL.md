---
name: cloth-language
description: Write and review Cloth source accepted by the C++ parser, including imports, functions, types and states, expressions, bindings, and control flow.
---

# Cloth syntax

<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->

Use the parser to trace accepted syntax and its `SourceUnit`. The ordinary `--src` invocation also builds the file's `SymbolTable`, resolves declarations and checks bodies across reachable imports. The backend receives the resulting `CheckedProgram`. [Parser](../../../Compiler/src/parser/parser.cpp) · [Semantic checking](../../../Compiler/src/semantic/checked_program.cpp) · [Driver](../../../Compiler/src/cloth.cpp)

Use the [language reference](../../../Documentation/language/index.rst) for program behavior. [Names and visibility](../../../Documentation/language/source/Visibility.rst) · [Calls and results](../../../Documentation/language/expressions/Calls.rst) · [Operator overloads and equality](../../../Documentation/language/expressions/Operators.rst) · [Arrays](../../../Documentation/language/types/Arrays.rst)

## Source files and functions

~~~cloth
using geometry.points: Point, Grid;

pub fn<int> offset(int x, final opt<int> step ?: 1) = x + step;
fn<int, int> neighbors(int x) = x - 1, x + 1;

fn main() {
    int low, int high = neighbors(8);
    def next := offset(low, step = 2);
}
~~~

- Put `using` imports in the file header. Write a dotted module path followed by `:` and either `*` or a comma-separated list of distinct names. End the import with `;`. The module path and selection may span lines. The parser records the import in the source unit. [Import parser](../../../Compiler/src/parser/subs/import_parser.cpp)
- Write a top-level function as `fn`, an optional angle-bracketed result type or type list, its name, parameters, and a body. Type parameters follow the name, as in `fn<T> first<T>(T value) = value;`. Parameters use `Type name`. The optional-argument spelling is `opt<Type> name ?: expression`, and `final` can precede a parameter. Named call arguments use `name = expression`. [Function parser](../../../Compiler/src/parser/subs/function_parser.cpp) · [Call arguments](../../../Compiler/src/parser/subs/expression_parser.cpp)
- Use a block or an equals body. A braced declaration ends at `}`. An equals body takes a semicolon, including when the body ends in a brace. Source files may consist entirely of helper functions or type declarations. [File grammar](../../../Compiler/src/parser/parser.cpp)

## More syntax

- For type spellings, generics, classes, structs, enums, interfaces, operators, and state declarations, read [Types and members](references/types-and-members.md).
- For literals, calls, indexing, interpolation, guarded access, providers, casts, and operator grouping, read [Expressions](references/expressions.md).
- For bindings, result lists, assignments, conditionals, and loop headers, read [Statements and control flow](references/statements.md).

For a spelling question, check the scanner's keyword and operator tables, then follow the grammar that consumes the token. Use the ordinary driver invocation to check a complete source file. [Token spellings](../../../Compiler/src/token/token.cpp) · [Frontend internals](../cloth-frontend/SKILL.md) · [Build and invocation](../cloth-repository/SKILL.md)
