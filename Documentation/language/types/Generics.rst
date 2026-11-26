.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Generic types and functions
===========================

Declare a type parameter after the name of a function, method or type.
Here the two uses of ``T`` in ``twice`` give the parameter and the result
the same type. Passing an ``int<8>`` keeps the result eight bits wide.

.. code-block:: text

   using cloth.lang.num.Number: Number;

   fn<T> twice<T: Number>(T value) = value + value;
   fn<int<8>> narrow(int<8> value) = twice(value);

Check this example with ``--stdlib Stdlib/StandardLibrary/src`` from the repository root.
The `numeric bound example <../../../Tests/Compiler/Semantic/generic-number.co>`_
also calls ``twice`` with unsigned integers, floats and a class that supplies ``+``.

The same function can be compiled to a native object for several types.
For example, ``twice<int<12>>(21)`` returns 42, and
``twice<unsigned int<8>>(150)`` returns 44 after eight-bit addition.
A class that implements the imported ``Number`` interface can supply
its own ``+`` for the same call. For a complete executable, see the
`linked specialization example <../../../Tests/Compiler/Driver/Inputs/generic-specialization.co>`_.

Arguments and inference
-----------------------

A declaration such as ``typedef<struct> Pair<A, B> { pub A left, pub B right }``
can be used as ``Pair<int, bool>``. The arguments are ordered and form
part of the type's identity. An instance's fields and methods use those
actual arguments wherever the declaration uses ``A`` or ``B``.

A call can infer type arguments from its supplied values, including the
element type of an array or the arguments of an implemented interface.
Typed arguments establish a type before an untyped numeric literal is
checked against it. A lone ``null`` or empty array supplies too little
information to infer its payload or element type. A destination type
alone cannot infer a class argument for a parameterless constructor.

Write ``twice<int<8>>(1)`` to choose a type explicitly. Keep the opening
angle bracket next to the name and the closing one next to the call's
opening parenthesis. A matching ordinary overload takes precedence over
an inferred generic overload.

A default can refer to an earlier type parameter. In this example
``choose<bool>(true, false)`` uses the default ``U = bool``.
``choose<bool, _>(true, n)`` instead infers ``U`` from ``n``.

.. code-block:: text

   fn<U> choose<T, U = T>(T a, U b) = b;
   fn<int<8>> chooseByte(int<8> n) = choose<bool, _>(true, n);

An explicit list can omit trailing parameters that have defaults. Use
a written ``_`` for a position that needs inference. The same distinction
applies to constructor calls such as ``Box<bool, _>::new(...)`` when the
class supplies a matching constructor.

Bounds
------

A bound declares the operations a generic body can use. A class or
interface bound gives access to its visible members. Join requirements
with ``&``. A constructor bound such as ``T: new()`` allows ``T::new()`` and
requires a matching accessible constructor on the supplied type.
For example, ``fn<T> make<T: new()>() = T::new();`` can construct any class
that meets that requirement.

When a generic body calls a constructor through its bound, argument names
and omitted defaults come from the bound. The selected concrete constructor
receives those values even if its own parameters have different names or defaults.

The standard library's ``Number`` admits signed and unsigned integers,
``isize``, ``usize``, and floating-point types. A class can explicitly implement
it and supply operators. ``twice<bool>(true)`` fails the bound with ``C3135``.
A separate interface also named ``Number`` has ordinary interface rules.

Nullable and array bounds describe the whole type argument. With
``T: Number?``, ``int?`` qualifies and ``int`` does not. Inside the body,
checking a value of ``T`` for ``null`` makes its numeric payload available.
With ``T: Number[]``, an open ``float[]`` qualifies. A fixed ``int[2]`` has
a different outer extent and fails that bound.

The compiler checks generic declarations even when they have no caller.
A recursive call with the same type arguments can reuse the instance.
Repeatedly growing the arguments, as in ``grow<T[]>()`` inside ``grow<T>``,
can exceed the dependency limit. See the `expanding recursion example
<../../../Tests/Compiler/Semantic/generic-expansion.co>`_ and the
`inference and constructor-bound tests
<../../../Tests/Compiler/Semantic/Unit/generic_instances_test.cpp>`_.
