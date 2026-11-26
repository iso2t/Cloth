.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

From source to a checked program
================================

An imported call such as ``identity(42)`` crosses two file scopes. The
frontend finds the public declaration, checks its signature and records
which parameter receives ``42``. The backend gets that decision along with
the syntax tree. The `two-file example
<../../../Tests/Compiler/Driver/Inputs/imported-calls/>`_ exercises the
handoff with a real object file.

Source and declarations
-----------------------

``SourceFile`` keeps the original UTF-8 text and path. The lexer keeps a
token's spelling and source span, alongside a normalized lookup key for
identifiers. Parsing builds a ``SourceUnit`` with header imports,
declarations, and the end-of-file span. Spans keep the source available
for diagnostics, including reports about an imported file. Use the
original token for a label and the lookup key for name comparisons.
See the `source unit <../../../Compiler/include/ast/source_unit.hpp>`_
and `diagnostic label conventions <../../../Diagnostics/README.rst>`_.

Declaration collection builds a ``SymbolTable`` for each file, including
the scopes of types and states. Overloads share a name and retain their
own signatures. The checker follows imports from the entry file and
indexes each source once. Each importing file sees its selected public
names. Resolving signatures across these scopes before checking bodies
allows a call to a later declaration and mutually recursive functions.
See `symbol collection <../../../Compiler/src/semantic/symbol_table.cpp>`_
and `import resolution <../../../Compiler/src/semantic/checked_program.cpp>`_.

Bodies, defaults, and instances
-------------------------------

The checker resolves names throughout a body, including code after an
unconditional transfer. It checks defaults in their declaration's
scope even when every caller supplies the argument explicitly. For
a call it records the chosen declaration and the mapping from actual
arguments to parameters. The mapping preserves the caller's written
evaluation order, including when named arguments appear out of order.

Generic declarations are checked against their bounds. A concrete
instance carries its substituted signature and body facts. The same
syntax node can have different types or call targets in different
instances. A separately declared state can likewise have different
member selections for each class that attaches it. Use the facts for the
instance or attachment being examined. See `generic instance checking
<../../../Compiler/src/semantic/instances.cpp>`_.

The checked program
-------------------

``CheckedProgram`` owns the file symbol tables and the declarations they
reference. Its maps use syntax nodes and symbols as keys. Keep these
objects alive at stable addresses while reading the maps. The program
also carries the selected target layout, struct layouts, dispatch tables,
and the entry point when checking an executable.

``ExpressionInfo`` describes ordered results and whether evaluation
can fall through. A ``void`` call can continue with no value, while
``exit(7)`` has no continuing path. Body facts also record the target of
a loop jump, the operation chosen for a ``for``, and any receiver checks
needed after evaluating arguments or an assignment's right-hand side.
These distinctions determine which side effects can reach the next
operation. See the `checked program definitions
<../../../Compiler/include/semantic/checked_program.hpp>`_ and the
`body-checking tests <../../../Tests/Compiler/Semantic/Unit/checked_program_test.cpp>`_.

Library intrinsics
------------------

A library declaration can bind to a compiler operation using a body such
as ``= intrinsic "text.utf8.decode"``. Registration checks the module under
the configured standard library root, the declaration and its signature.
Copying the spelling into an application does not register a new intrinsic.
The `program boundary checks <../../../Compiler/src/semantic/program_boundaries.cpp>`_
also select the entry point for executable checking. For what the selected
program can emit, see `Lowering a checked program <Backend.rst>`_.

The checker also loads ``cloth/lang/num/Number.co`` from the selected
standard library when that module exists. The public ``primitive.integer``
and ``primitive.float`` declarations supply members for numeric receivers.
Member lookup binds each declaration's implicit ``Self`` parameter
to the receiver's resolved type. The generic instance key carries that type
through body checking and native code generation. The checker diagnoses
invalid constant arguments at the call site. Builds without a binding for
the family use the standalone compiler's numeric methods. See the
`numeric declarations <../../../Stdlib/StandardLibrary/src/cloth/lang/num/Number.co>`_.
