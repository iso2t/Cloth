.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Member access and data providers
================================

Use ``.`` to read a field or call a method. When a receiver can be ``null``,
use ``?.`` to perform the access only when the receiver is present.

.. code-block:: text

   fn<usize?> find(string? text) = text?.indexOf("needle")
   fn<bool> hasNeedle(string? text) = text?.contains("needle") ?: false

An absent ``text`` skips the call and its arguments. In ``find``, the result
is ``usize?`` whether the receiver is absent or the search yields no index.
A guard turns a single plain result into a nullable one and preserves an
already nullable result. In ``hasNeedle``, the fallback supplies ``false``
for an absent string. A call with no results can be guarded as a statement.
A call with several results cannot be used this way.

A check such as ``if (item != null)`` lets the branch use ``item`` directly.
The same rule applies to a field or a known array slot while the compiler
can establish that it still holds the checked value. After an assignment
or a call that can change that location, check it again before reading
its payload. See the `nullable access examples
<../../../Tests/Compiler/Semantic/Unit/flow_test.cpp>`_.

Guarded field reads
-------------------

Use ``sample?.value ?: 7`` to read an integer field from a nullable
struct. The receiver is evaluated once. A present field holding 42 or 0
returns that value. If ``sample`` is null, the field read is skipped and
the fallback supplies 7. See the `guarded field example
<../../../Tests/Compiler/Driver/Inputs/guarded-struct-field.co>`_.

If the field itself can be null, its absence also selects the fallback.
In ``maybe?.sample?.value``, each guard checks its own receiver. The
`nested example <../../../Tests/Compiler/Driver/Inputs/guarded-struct-field-values.co>`_
also shows when a fallback with a side effect runs.

Data providers
--------------

Write ``::`` to ask a type or a value for information. The receiver
determines what the answer measures.

* ``int<12>::size`` gives the storage size in bytes. Integers, floats,
  booleans, and characters have a storage size. On a nullable scalar,
  the size includes its presence representation. The query also accepts
  nullable types such as ``Mode?`` for an enum, ``Object?``, and ``typeinfo?``.
  See `Nullable storage
  <../../compiler/storage/Aggregates.rst>`_.
* ``int[2][3]::size`` gives 2, the outer extent. On an array value,
  ``values::size`` gives the current outer length. An array type with an
  open outer dimension has no fixed count to query.
* On a string value, ``text::size`` returns the number of Unicode scalar
  values as a ``usize``. An empty string has size 0 and ``"Cloth"`` has
  size 5. In ``"é\0😀"``, the accented letter, embedded NUL, and emoji
  each count once. A combining mark also counts separately from its base
  letter. See the `string size example
  <../../../Tests/Compiler/Driver/Inputs/string-size-values.co>`_.
* ``int<8>::min`` and ``int<8>::max`` give -128 and 127. Integers and floats
  supply both limits. Querying a nullable numeric value this way requires
  its payload to be present.
* ``value::str`` formats a value as a ``string``. A type can provide its own
  representation with ``operator ::str() -> string``. The same formatter
  supplies text for `string interpolation <../source/Source.rst>`_.
* For an enum ``Mode``, ``Mode::values`` gives an array of its cases in
  declaration order. On a case value, ``mode::name`` gives its name.

Type information
----------------

``Type::type`` and ``value::type`` produce a ``typeinfo`` value. On a binding,
the query describes the declared type, including nullable markers and fixed
extents. Use a second ``::`` to inspect it.

.. code-block:: text

   fn<usize> rows() = int[2][3]::type::extent
   fn<usize> columns() = int[2][3]::type::element::extent
   fn<bool> unsignedByte() = unsigned int<8>::type::isunsigned

``::isarray`` and ``::hasextent`` test for an array and a known outer extent.
``::rank`` counts the dimensions. ``::element`` removes the outer dimension
and returns another ``typeinfo``. ``::extent`` requires an array with a
statically known outer extent. The two queries above give 2 and 3.

``::isnumeric`` and ``::isunsigned`` are boolean queries. ``::width`` gives
the bit width of an integer or float as a ``usize``. For examples of invalid
receiver types, see the `provider tests
<../../../Tests/Compiler/Semantic/Unit/value_semantics_test.cpp>`_.
