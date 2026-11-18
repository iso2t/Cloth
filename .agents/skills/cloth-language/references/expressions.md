<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->

# Expressions

## Literals and interpolation

~~~cloth
fn<string> describe(int? count) {
    def value := count ?: 0
    def<int[3]> sizes = [0b10, 0o10, 0x10]
    def<char> mark := 'λ'
    def raw := """{literal}"""
    return #"{{{value}}} {sizes[0]} {mark} {raw}"
}
~~~

- Use decimal digits or `0b`, `0o`, and `0x` integer prefixes. Underscores separate digits. Decimal literals accept an integer suffix (`12i`) or a float suffix (`12f`, `1.25F`). The sign is a unary operator. [Numeric scanner](../../../../Compiler/src/lexer/lexer.cpp) · [Literal decoding](../../../../Compiler/src/token/numeric_literals.cpp)
- A character literal holds one Unicode scalar or escape. Double quotes delimit ordinary strings. Triple double quotes preserve the raw contents, including newlines. In `#"..."` an expression goes inside `{...}`, and doubled braces spell literal braces in the text. Ordinary strings and character literals accept escapes such as `\n` and `\\`. [Lexer](../../../../Compiler/src/lexer/lexer.cpp) · [Interpolation parser](../../../../Compiler/src/parser/subs/expression_parser.cpp)
- Square brackets delimit an array literal. A bracket after an expression indexes it. Braces in expression position hold comma-separated struct field values. A brace initializer directly after a binding operator has extra disambiguation rules. [Primary and postfix expressions](../../../../Compiler/src/parser/subs/expression_parser.cpp) · [Binding forms](statements.md)

## Calls, access, and casts

~~~cloth
typedef<class> Box(public def<int> value) {
    mt<int> read() = self.value
}

fn<T> first<T>(T value) = value

fn inspect(Box? box) {
    def value := box?.read() ?: 0
    def copy := first<int>(value)
    def widened := copy -> float
    def sameType := widened is float
    def width := int::size
    def other := Box::new(value)
    other::delete
}
~~~

A dot selects a field or method. `?.` starts a guarded member chain. With `::`, an identifier alone produces a provider access and an identifier with arguments produces a state call. The keywords `new` and `delete` in that position have dedicated AST nodes. The parser records provider and method names for later resolution. [Postfix forms](../../../../Compiler/src/parser/subs/expression_parser.cpp) · [Provider nodes](../../../../Compiler/include/ast/expr/provider_expressions.hpp)

`value -> Type` parses as a cast. An identifier with parentheses after the arrow, as in `self -> On()`, parses as a state transition with an optional payload expression. `is` has dedicated type-check and expression-target parsing. [Arrow and type-check parsing](../../../../Compiler/src/parser/subs/expression_parser.cpp)

## Operator grouping

Among ordinary binary operators, precedence rises from `?:` through `||`, `&&`, equality (`==`, `!=`, `===`), comparisons, addition and subtraction, multiplication, division and remainder, then `**`. The parser gives `..` and `..=` precedence between comparisons and addition, with `->` between ranges and addition. `?:` and `**` group to the right. Power binds inside a leading sign, so `-2 ** 2` groups as `-(2 ** 2)`. Use parentheses to express a different grouping. [Precedence table and infix parser](../../../../Compiler/src/parser/subs/expression_parser.cpp) · [Cast and power precedence](../../../../Compiler/include/parser/subs/expression_parser.hpp)
