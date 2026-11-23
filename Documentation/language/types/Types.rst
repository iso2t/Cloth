.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Primitive and nullable types
============================

Use ``int?`` when an integer can be absent. Zero is a present value.
The `nullable integer example
<../../../Tests/Compiler/Driver/Inputs/nullable-integer.co>`_ returns 42
after assigning ``null`` to ``value``.

.. code-block:: text

   pub fn<int> main() {
       int? value = 0
       if (value == null || value != 0) return 1
       value = null
       return if (value == null) 42 else 2
   }

Numeric types
-------------

``int`` is a signed 32-bit integer. Write ``int<N>`` or
``unsigned int<N>`` for a width from 1 through 65,536 bits. For example,
``int<12>`` ranges from -2048 through 2047 and ``unsigned int<12>``
ranges from 0 through 4095. Use ``int<12>::min`` or ``int<12>::max``
to name a limit directly.

``isize`` and ``usize`` are the signed and unsigned integers whose width
matches the target pointer. A ``usize`` is 32 bits on a 32-bit target
and 64 bits on a 64-bit target. For target selection and the space an
integer occupies, see `Target width <../../compiler/Flags.rst#target-width>`_
and `Integer storage layout <../../compiler/storage/Integers.rst>`_.

``float`` is a 32-bit floating-point type. The explicit widths are
``float<16>``, ``float<32>``, and ``float<64>``. Arithmetic uses the
operand type, and a cast names its destination type. See
`Numeric conversions <../expressions/Operators.rst#numeric-conversions>`_.

Booleans and text
-----------------

A ``bool`` holds ``true`` or ``false``. A ``char`` holds one Unicode
scalar value, including ``'\0'``. Use single quotes for a character and
double quotes for a ``string``, as in ``'A'`` and ``"Cloth"``.

Other built-in types
--------------------

``void`` names an empty result list, as in ``fn<void> reset() {}``.
``Object`` is the root class type. A class instance can be passed to an
``Object`` parameter. For operations on the resulting reference, see
`Object ownership <../objects/Ownership.rst>`_.

``typeinfo`` holds a description such as ``int<12>::type``.
For querying a width, array rank, or extent, see `Type information
<../expressions/Access.rst#type-information>`_. The built-in
``range<T>`` type holds a numeric range. See `Ranges and membership
<../expressions/Operators.rst#ranges-and-membership>`_.

Nullable values
---------------

Append ``?`` to the type that can be absent. A nullable value is either
``null`` or a value of its underlying type. Testing ``value == null``
checks for absence. A present ``false``, ``0.0``, or ``'\0'`` keeps its
value. The same rule applies to the first enum case and to a struct
whose fields are all zero.

Use ``value ?: fallback`` to supply a value when the left operand is
absent. For evaluation order, see `Evaluation and updates
<../expressions/Operators.rst#evaluation-and-updates>`_. For the position
of ``?`` in an array type, see `Rank and extents <Arrays.rst#rank-and-extents>`_.
To call a method only when its receiver is present, see `Member access
<../expressions/Access.rst>`_.
