Cloth Compiler
==============

The compiler's front end starts with **lexing**, which turns Cloth source text
into tokens such as keywords, identifiers, and operators. **Parsing** builds a
syntax tree from those tokens to represent the structure of the program.

**Declaration collection** builds the source unit's symbol table before body
checking. Top-level functions and types enter the file scope. Each type or
state has a scope for its own members and nested declarations. Callable
overloads retain their signatures for later name resolution and type checking.

Declarations are private by default. Each public declaration needs its own
``pub``, including members of a public type. ``private`` is an ordinary
identifier. Write ``int pin`` for a private struct field and ``pub int pin``
for a public one.

**Semantic checking** resolves declaration signatures across the entry file
and its imports before checking any bodies. This lets functions call later
declarations and each other. A dotted ``using`` path is relative to the entry
file's directory and selects a ``.co`` or ``.clib`` file. Imports expose only
declarations marked ``pub``. A private top-level name is visible throughout
its file, and a private member is visible inside its declaring type.

Inside a body, parameters and local bindings take precedence over members
and file-level names. Overload resolution uses argument labels, parameter
types, generic arguments and optional parameters. Exact matches take
priority over assignable matches and contextual literal conversions. The
checker validates assignments, conditions and return values, including
whether a value-returning function can reach its closing brace.

Explicit call arguments are evaluated in written order, including named
arguments. Omitted defaults follow in parameter order and use names from
their declaring callable. A default can read earlier parameters. A virtual
method keeps the parameter labels and defaults of its original declaration.

A function can return zero, one, or several ordered values. A conditional
used as a value checks each continuing arm against the same result count and
types. An arm that returns from the function contributes no values to the
join. Grouped assignments receive multiple results by position. A single
result can supply every target.

An ordinary ``::new`` expression selects an accessible constructor overload.
A secondary constructor delegates to the primary constructor of its own class.
The checker visits field initializers in declaration order. An initializer can
read constructor inputs, accessible inherited fields, and earlier fields of the
same instance. The secondary constructor body can use the fully initialized
``self`` after delegation.

The backend receives a ``CheckedProgram`` with resolved declarations, chosen
overloads, argument bindings and expression result types. LLVM emission is
still pending.
