.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Calls and results
=================

Overload resolution uses argument labels, parameter types, generic arguments
and optional parameters. Exact matches take priority over assignable matches
and contextual literal conversions.

Explicit arguments are evaluated in written order, including named
arguments. Omitted defaults follow in parameter order and use names from
their declaring callable. A default can read earlier parameters. A virtual
method keeps the parameter labels and defaults of its original declaration.

Return values and bindings
--------------------------

A function can return zero, one, or several ordered values. Falling through
the closing brace of a function with declared results is an error. When a
conditional is used as a value, each continuing arm must produce the same
number of results with compatible types. An arm that returns from the
function contributes no values to the conditional.

Grouped assignments receive multiple results by position. A single result
can supply every target. In a deferred binding such as
``def<int, bool> n, ok = { return 1, true }``, each read evaluates the whole
block and selects the result at that name's position. Reading ``n`` and then
``ok`` runs the block twice, including any writes to captured variables.
A discard (``_``) occupies a result position without introducing a name.

Constructors
------------

An ordinary ``::new`` expression selects an accessible constructor overload.
A secondary constructor delegates to the primary constructor of its own class.
Field initializers run in declaration order. An initializer can read
constructor inputs, accessible inherited fields, and earlier fields of the
same instance. The secondary constructor body can use the fully initialized
``self`` after delegation.
