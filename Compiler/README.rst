Cloth Compiler
==============

The compiler's front end starts with **lexing**, which turns Cloth source text
into tokens such as keywords, identifiers, and operators. **Parsing** builds a
syntax tree from those tokens to represent the structure of the program.

**Declaration collection** builds the source unit's symbol table before body
checking. Top-level functions and types enter the file scope. Each type or
state has a scope for its own members and nested declarations. Callable
overloads retain their signatures for later name resolution and type checking.

**Semantic checking** resolves declaration signatures across the entry file
and its imports before checking any bodies. This lets functions call later
declarations and each other. Within each body the checker resolves names and
overloads, checks types, and analyzes control flow.

The backend receives a ``CheckedProgram`` with resolved declarations, chosen
overloads, argument bindings and expression result types.

Language reference
------------------

* `Names and visibility <../Documentation/compiler/Visibility.rst>`_
* `Calls and results <../Documentation/compiler/Calls.rst>`_
* `Operator overloads and equality <../Documentation/compiler/Operators.rst>`_
