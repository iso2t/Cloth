<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->

# Types and members

## Type references and parameters

The primitive type spellings are `bool`, `char`, `string`, `int`, `float`, `void`, `typeinfo`, `usize`, and `isize`.
Numeric annotations include `unsigned int<8>` and `float<32>`. A named type can have a dotted path and a type argument
list. In a declaration, `?` marks the position immediately before it. For example, `int?[]` marks the element and
`int[]?` marks the array. `int[4][]` records two separate array
suffixes. [Type parser](../../../../Compiler/src/parser/subs/type_parser.cpp) · [Type representation](../../../../Compiler/include/ast/type/type_reference.hpp)

Type parameters follow a declaration name. A parameter can have bounds joined by `&`, a constructor bound such as
`new()`, or a default introduced with `=`. The parser also accepts `_` in a type argument list. Explicit call type
arguments require the name to touch `<` and the closing `>` to touch `(`, as in
`first<int>(value)`. [Parameters and call lookahead](../../../../Compiler/src/parser/subs/type_parser.cpp) · [Type parameter nodes](../../../../Compiler/include/ast/type/type_parameters.hpp)

## Structs, enums, interfaces, and classes

~~~cloth
typedef<struct> Pair<T> {
    T left, T right;
    mt<T> first() = self.left
}

typedef<enum> Mode(pub def<int> code) { Off(0), On(1) }

typedef<interface> Sized {
    mt<int> length()
    mt<bool> empty() = default (self.length() == 0)
}

pub final typedef<class> Counter(pub def<int> value) : Sized {
    Counter() : (0) {}
    pub mt<int> length() = self.value
    operator +(int delta) -> int = self.value + delta
    ~Counter {}
}
~~~

- In a struct, write comma-separated `Type name` fields. A semicolon separates the field list from following methods or
  operators. In an enum, put payload fields in the header and supply their values on the cases. Each enum needs at least
  one
  case. [Struct parser](../../../../Compiler/src/parser/subs/struct_parser.cpp) · [Enum parser](../../../../Compiler/src/parser/subs/enum_parser.cpp)
- Use `mt` for a method. An interface accepts a signature, `= default (expression)`, or `= default { ... }`. A class or
  interface parent list starts with `:`. Class parents may also take constructor
  arguments. [Interface parser](../../../../Compiler/src/parser/subs/interface_parser.cpp) · [Method bodies](../../../../Compiler/src/parser/subs/function_parser.cpp) · [Parent clauses](../../../../Compiler/include/parser/subs/type_parser.hpp)
- A class header can hold ordinary parameters and `def<Type>` field parameters. A class-body field uses
  `def<Type> name = expression`. A secondary constructor repeats the class name and delegates with `: (arguments)`. The
  destructor is `~Name` followed directly by a block. Class methods accept `virtual:` or `override:`
  prefixes. [Class parser](../../../../Compiler/src/parser/subs/class_parser.cpp)
- `operator` takes the operator spelling, parameters, an optional `->` result type, and a body. Nested type declarations
  use the same `typedef<kind>` header. Consult the enclosing member parser for valid positions and
  separators. [Operator declarations](../../../../Compiler/src/parser/subs/function_parser.cpp) · [Nested type parsing](../../../../Compiler/src/parser/parser.cpp)

## Class states

~~~cloth
typedef<class> Lamp {
    state<void> ::toggle()
    default: state<void> Off {
        mt toggle() { self -> On() }
    }
    state On
}

typedef<state<void>> pub On for Lamp {
    mt toggle() { self -> Off() }
}
~~~

Within a class, `state<void> ::toggle()` declares a state-method signature. `state<Type> Name { ... }` declares a nested
state, and `state Name` attaches a separately declared state. A `default:` prefix marks an initial state and can carry a
parenthesized payload. A top-level state places its optional `pub` after `typedef<state<Type>>` and names its host
with `for`. State bodies accept `init` blocks and `mt` methods. In a state method, `state` can appear as a payload
expression. [State grammar](../../../../Compiler/src/parser/subs/class_parser.cpp) · [State nodes](../../../../Compiler/include/ast/decl/state_declaration.hpp) · [State expressions](../../../../Compiler/src/parser/subs/expression_parser.cpp)
