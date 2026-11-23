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

An ``intrinsic`` body binds a standard library declaration to a compiler
operation. The checker verifies the declaring module under the configured
standard library root and the full signature of the declaration.

The backend receives a ``CheckedProgram`` with resolved declarations, chosen
overloads, argument bindings and expression result types. When the driver
requests an object, the backend lowers the entry point and its reachable
helpers to LLVM IR, verifies the module, and emits the target object.

* `From source to a checked program <../Documentation/compiler/internals/Frontend.rst>`_
  describes file scopes, body checking, and the facts attached to each expression.
* `Lowering a checked program <../Documentation/compiler/internals/Backend.rst>`_
  covers the target ABI, control transfers, runtime calls, and object output.
* `Diagnostic codes and source labels <../Diagnostics/README.rst>`_ explains how
  to report an error from either part of the compiler.

For compiler options, object files, and target layout, see the
`compiler manual <../Documentation/compiler/index.rst>`_.

Language reference
------------------

For names, calls, operators, and control flow, see the
`Cloth language reference <../Documentation/language/index.rst>`_.
