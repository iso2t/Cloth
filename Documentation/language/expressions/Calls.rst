.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Calls and results
=================

Declaring a callable
--------------------

Use ``fn`` at file scope and ``mt`` for a method. List result types in angle
brackets and write each parameter as its type followed by its name. An
expression body starts with ``=``. A block body can use ``return``.

.. code-block:: text

   fn<int> shift(int value, opt<int> amount ?: 1) = value + amount

   typedef<struct> Offset {
       pub int delta;
       pub mt<int> apply(final int value) {
           return shift(amount = delta, value = value)
       }
   }

``shift(4)`` yields 5. ``shift(amount = 3, value = 4)`` yields 7.
``opt<int> amount ?: 1`` provides a default when the caller omits ``amount``.
Write ``final int value`` to prevent reassignment of that parameter.
For a type parameter or an explicit type argument, see `Generic types
and functions <../types/Generics.rst>`_.

Binding arguments
-----------------

Overload resolution uses argument labels, parameter types, generic arguments
and optional parameters. Exact matches take priority over assignable matches
and contextual literal conversions.

An integer argument can supply a ``bool`` parameter when no overload
matches without that conversion. Given ``pick(int)`` and ``pick(bool)``,
``pick(0)`` selects the integer overload and ``pick(false)`` selects the
boolean overload.

Explicit arguments are evaluated in written order, including named
arguments. Omitted defaults follow in parameter order and use names from
their declaring callable. A default can read earlier parameters.
A named argument uses the parameter name followed by ``=``. Positional
and named arguments can be mixed, but each parameter takes at most one
argument. See the `duplicate argument diagnostic
<../../compiler/driver/Diagnostics.rst>`_.

A base class declares a virtual method with ``pub virtual mt``. A derived
class can supply a different body with ``pub override mt``. Calls use the
parameter labels and defaults from the original virtual declaration.

Return values and bindings
--------------------------

A function can return zero, one, or several ordered values. Falling through
the closing brace of a function with declared results is an error. When a
conditional is used as a value, each continuing arm must produce the same
number of results with compatible types. An arm that returns from the
function contributes no values to the conditional.

A call can stand alone as a statement. A ``void`` helper returns at the
end of its block or at a bare ``return``. Here ``touch()`` returns to
``main``, which returns 7.

.. code-block:: text

   pub fn<int> main () {
       int answer = 42
       answer = 7
       touch()
       return answer
   }

   fn<void> touch() {}

To bind several results by position or discard one with ``_``, see
`Receiving several results <../statements/Bindings.rst#receiving-several-results>`_.
A block initializer can also provide its results on each read. See
`Deferred bindings <../statements/Bindings.rst#deferred-bindings>`_.

Constructors
------------

An ordinary ``::new`` expression selects an accessible constructor overload.
A class header takes ``Type name`` for a constructor parameter and
``def<Type> name`` for a parameter stored as a field.
The primary constructor is private. A public secondary constructor gives
outside callers a way to create the class. For example, an empty
``List<int>::new`` calls the public zero-argument constructor, which supplies
an empty array to the private primary constructor. See the `List declaration
<../../../Stdlib/cloth/collections/List.co>`_.

A secondary constructor delegates to the primary constructor of its own class.
Field initializers run in declaration order. An initializer can read
constructor inputs, accessible inherited fields, and earlier fields of the
same instance. The secondary constructor body can use the fully initialized
``self`` after delegation. For the lifetime of the resulting reference,
see `Object ownership <../objects/Ownership.rst>`_.
