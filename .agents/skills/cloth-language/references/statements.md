<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->

# Statements and control flow

## Bindings, assignments, and results

~~~cloth
typedef<struct> Point { int x, int y; }

fn<int, int> limits() = 1, 4;

fn update() {
    int low, int high = limits();
    _, high = limits();
    Point point = {low, high};
    int[2] pair = [point.x, point.y];
    pair[1] *= 2;
    point.x++;
    int later := { return high; };
}
~~~

`def name = value` declares a mutable binding, and `def name := value` declares an immutable one. An explicitly typed local starts with its type, as in `int<12> x = 1`. For several results, write a type before each target. A single shared type, as in `int low, high = 0`, assigns one scalar to both names. A comma-separated target list can include `_` for a discarded position. Returns and equals bodies also accept comma-separated result expressions. The parser retains the target and result lists for later checking. [Binding parser](../../../../Compiler/src/parser/subs/bindings_parser.cpp) · [Results and receiving targets](../../../../Compiler/src/parser/parser.cpp)

Use `=` for reassignment. Names, fields, and indexed places also accept `+=`, `-=`, `*=`, `/=`, `%=` and standalone postfix `++` or `--`. A binding initializer has two brace forms. In the example, `{low, high}` constructs a struct-literal node, and `{ return high; }` constructs a deferred function-block node. The binding annotation and brace contents guide the parser. [Statement and binding-brace parsing](../../../../Compiler/src/parser/parser.cpp)

## Conditions and loops

~~~cloth
fn<int> total(int[] values) {
    def sum = 0;
    for (def value <- values) sum += value;
    for (int i <- 0..=2; i++) {
        if (i == 1) continue;
        sum += i;
    }
    while (sum < 3) { sum++; }
    do { sum--; } while (sum > 9);
    return if (sum < 0) { 0; } else { sum; };
}
~~~

- `if (condition)` accepts a block or a single statement, followed by optional `else if` branches and an optional `else`. It is also a primary expression, as in the return above. [Conditional parser](../../../../Compiler/src/parser/subs/if_parser.cpp) · [Expression parser](../../../../Compiler/src/parser/subs/expression_parser.cpp)
- A `for` header binds an inferred `def name` or an explicit `Type name`, followed by `<-`. An outer `..` or `..=` introduces a range. A range may add `;` followed by an update targeting the same binding. An element loop ends its header after the source expression. [For parser](../../../../Compiler/src/parser/subs/loops/for_loop_parser.cpp)
- `while` takes a parenthesized condition before its body. `do` takes its body first and ends with `while (condition);`. Both forms accept braced or single-statement bodies. `break` and `continue` produce jump statements. [While](../../../../Compiler/src/parser/subs/loops/while_loop_parser.cpp) · [Do-while](../../../../Compiler/src/parser/subs/loops/do_while_loop_parser.cpp) · [Shared loop bodies](../../../../Compiler/src/parser/subs/loops/loop_parser.cpp)
- End each simple statement with `;`, including the last one before `}`. Braced controls end at their closing brace. A braced conditional value arm ends its result list with `;`, and the enclosing binding or return takes its own terminator. [Statement grammar](../../../../Compiler/src/parser/parser.cpp) · [Struct grammar](../../../../Compiler/src/parser/subs/struct_parser.cpp)
