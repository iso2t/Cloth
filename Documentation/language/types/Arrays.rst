.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Arrays
======

Assigning a fixed-size array copies its elements. In this example,
``values[1]`` becomes 42 and ``saved[1]`` still holds 9.

.. code-block:: text

   pub fn<int> main() {
       int<12>[2] values = [7, 9]
       def saved := values
       values[1] = 42
       if (saved[1] != 9 || values[0] != 7) return 1
       return values[1] -> int
   }

The program returns 42. The ``:=`` makes ``saved`` immutable. The ``=``
in the declaration of ``values`` allows its elements to be updated. See
the `complete example <../../../Tests/Compiler/Driver/Inputs/fixed-array-write.co>`_.

Rank and extents
----------------

Write the element type followed by one pair of brackets per dimension.
The number of dimensions is the array's rank. A number in brackets fixes
the length of that dimension. Empty brackets leave its length unspecified.
A mutable ``int[]`` can grow. ``values::size`` gives the number of
elements in the outer dimension.

Dimensions are written from the outside in. ``int[2][3]`` holds two rows
of three integers each. In ``int[][] rows = [[1, 2]]``, the first row
establishes a length of two for later rows. Assigning
``[[3, 4], [5, 6]]`` to ``rows`` changes the number of rows while
preserving their length.

Use one index per dimension. In the `rectangular array example
<../../../Tests/Compiler/Driver/Inputs/rectangular-array.co>`_, the second
row is ``[4, 5, 42]``, so ``rows[1][2]`` returns 42. Cloth checks the row
index before evaluating the column index. If the selected row is empty,
every column index is out of bounds.

Nullable arrays
---------------

A ``?`` marks the type immediately before it. In ``int?[]``, each
element can be ``null``. In ``int[]?``, the array itself can be ``null``.
For example, ``int?[2]`` can hold ``[0, null]``.

An ``int[2]?`` local can hold ``null`` or a pair of integers. Assigning
``[0, 0]`` makes the pair present. Inside ``if (pair != null)``, you can
read either cell and update it if ``pair`` is mutable. Copying the local
preserves its presence and cells. In the `nullable array example
<../../../Tests/Compiler/Driver/Inputs/nullable-fixed-array.co>`_, clearing
the original leaves the copy present with both zeros.

A present empty array also passes a presence check. Its length is zero,
so iteration skips the body and every index is out of bounds.

Growable arrays
---------------

Use an open outer dimension for an array whose length can change.
In ``int[] values = [1, 2]`` the declared type allows growth.
``def values = [1, 2]`` infers a fixed two-element array.

.. code-block:: text

   fn<int[]> takeRow(int[][] rows) {
       rows.append([1, 2])
       rows.insert(0, [3, 4])
       rows.reserve(8)
       def row = rows.removeAt(1)
       rows.clear()
       return row
   }

``append(value)`` adds one element. ``insert(index, value)`` inserts at a
``usize`` index, and ``removeAt(index)`` returns the removed element.
``clear()`` removes all elements. ``reserve(count)`` requests capacity without
changing the length. All five operations require writable growable storage.

For an array of rows, inserting or appending a row must preserve the
established inner shape. Clearing the outer array preserves that shape.
An indexed row such as ``rows[0]`` can reserve capacity, but its length
cannot change independently of the other rows. See the `array mutation tests
<../../../Tests/Compiler/Semantic/Unit/value_semantics_test.cpp>`_.

Indexing and updates
--------------------

An array literal evaluates its elements from left to right. An indexed
read ``values[i]`` evaluates the array before the index. Indices are
integers starting at zero. Reading or writing with a negative index
or an index at or beyond ``values::size`` reports
``Cloth runtime error: array index out of bounds``. An empty array has
no valid index.

An indexed assignment to a mutable array local evaluates the index once and
checks its bounds before evaluating the right-hand side. A compound
assignment also reads the old element before evaluating the right-hand
side, then writes the result to the same slot. Numeric elements support
``+=``, ``-=``, ``*=``, ``/=``, ``%=``, and standalone ``++`` and ``--``.

For ``rows[r][c] = value``, Cloth evaluates and checks ``r`` first, then
``c``, and evaluates ``value`` only after both indices are in bounds. In
the `nested assignment example
<../../../Tests/Compiler/Driver/Inputs/rectangular-array-write.co>`_,
``rows[1][2] = 7`` changes one cell. The earlier copy still holds 42 at
``saved[1][2]``, and the program returns 7.

The expression ``values[index()] += 2`` calls ``index()`` once. In the
65-bit example the call prints one line containing ``1``, and the update
changes the selected element from 40 to 42. See the `indexed update
example <../../../Tests/Compiler/Driver/Inputs/fixed-array-update.co>`_.

For comparison with ``==`` and ``!=``, see `Array equality
<../expressions/Operators.rst#array-equality>`_. For the values a
``for`` loop visits, see `Fixed-size array iteration
<../statements/ControlFlow.rst#fixed-size-array-iteration>`_.

For native array support and the byte distance between integer elements,
see `Fixed-size arrays in object files
<../../compiler/driver/Objects.rst#fixed-size-arrays>`_ and
`Integer storage layout <../../compiler/storage/Integers.rst>`_.
