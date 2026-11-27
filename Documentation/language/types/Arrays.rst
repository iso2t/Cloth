.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Arrays
======

Assigning a fixed-size array copies its elements. In this example,
``values[1]`` becomes 42 and ``saved[1]`` still holds 9.

.. code-block:: text

   pub fn<int> main() {
       int<12>[2] values = [7, 9];
       def saved := values;
       values[1] = 42;
       if (saved[1] != 9 || values[0] != 7) return 1;
       return values[1] -> int;
   }

The program returns 42. The ``:=`` makes ``saved`` immutable. The ``=``
in the declaration of ``values`` allows its elements to be updated. See
the `complete example <../../../Tests/Compiler/Driver/Inputs/fixed-array-write.co>`_.

When an element is a class reference, copying the array copies the
reference. The corresponding cells refer to the same instance, so
``copy[0] === items[0]`` is true immediately after the copy. Replacing
``items[0]`` with another instance leaves ``copy[0]`` pointing to the
first one. See the `class array copy example
<../../../Tests/Compiler/Driver/Inputs/class-array-copy.co>`_.

Fixed-size array fields
-----------------------

Copying a struct also copies the elements of its fixed-size array fields.
In the `array field example <../../../Tests/Compiler/Driver/Inputs/fixed-array-fields.co>`_,
``sample.cells[1] = 7`` updates the original struct. Its earlier copy
still holds 42 in that cell. Assigning a whole array to the field also
copies its elements. A rectangular field keeps the same row and column
order as an array local.

A nullable field such as ``int<12>[2][2]? rows`` can hold a two-by-two
array or ``null``. Clearing ``sample.rows`` leaves an earlier struct
copy's cells available. Reassigning a two-by-two array makes the field
present again. A present array of empty rows also survives a copy and
passes a presence check. See the `nullable array field example
<../../../Tests/Compiler/Driver/Inputs/nullable-fixed-array-fields.co>`_.

A fixed-size array of structs copies the fields of every element. In the
`array of structs example <../../../Tests/Compiler/Driver/Inputs/array-of-structs.co>`_,
``Row[1] saved := rows`` copies the row and its two cells. Replacing
``rows[0]`` makes ``rows[0].cells[1]`` read 7 while
``saved[0].cells[1]`` still reads 42. You can also update a single cell
directly with ``rows[0].cells[1] = 7``. An element bound by a
``for`` loop has its own copy of the cells.

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

Copying ``int[][]`` copies every row. After ``def saved := rows;``,
a write to ``rows[1][1]`` leaves ``saved[1][1]``
at its original value. Reading a row into a separate array local also
copies its cells. See the `runnable growable matrix example
<../../../Tests/Compiler/Driver/Inputs/growable-rectangular-array.co>`_.

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

A struct can hold a nullable growable array. If ``row.cells`` holds
``[42]`` when you copy ``row``, assigning ``row.cells = null`` leaves
the copy's field present with one element, ``42``. Assigning ``[]``
to the field makes it present with length zero. See the `growable
field example <../../../Tests/Compiler/Driver/Inputs/nullable-growable-array-fields.co>`_.

A fixed-size array can hold nullable rows. For example, ``int[3]?[2]``
holds three rows, each either ``null`` or a pair of integers. Assigning
``rows[i] = null`` clears that row. Assigning a pair makes it present
again. A copy keeps the presence and cells of every row. In the
`nullable row example <../../../Tests/Compiler/Driver/Inputs/nullable-array-rows.co>`_,
the second row in the saved array still holds 42 after the original row
is cleared.

Bind a row with ``int[2]? row := rows[i]``, then test ``row != null``
before indexing it. To visit each row, write ``for (int[2]? row <- rows)``.
The loop also visits absent rows. A present row of length zero passes
the presence test and has no cells to visit.

Growable arrays
---------------

Use an open outer dimension for an array whose length can change.
In ``int[] values = [1, 2]`` the declared type allows growth.
``def values = [1, 2]`` infers a fixed two-element array.

In the `growable integer example
<../../../Tests/Compiler/Driver/Inputs/growable-array.co>`_, ``values``
starts with one element, 7. After ``values.append(42)`` its
``::size`` is two and ``values[1]`` returns 42. To start empty,
write ``int[] values = []``. Calling ``values.reserve(8)`` reserves
room for at least eight elements and leaves the current length intact.

The same operations work on booleans, characters, and floats. For
example, inserting ``'\0'`` between two characters increases the length
by one, and ``removeAt`` returns that character. See the `growable scalar
array example <../../../Tests/Compiler/Driver/Inputs/growable-scalar-arrays.co>`_.

In ``int?[] values = [null, 0]``, both cells count toward the length.
Removing the first returns ``null``. Removing the second returns a
present zero, so ``?:`` keeps the zero. Copying the array preserves
each cell's presence. Use an explicitly nullable binding such as
``for (int? value <- values)`` when iterating. See the `nullable cell
example <../../../Tests/Compiler/Driver/Inputs/growable-nullable-cells.co>`_.

If a growable array holds structs, their array fields copy recursively.
For example, after ``Row[] saved := rows``, changing
``rows[0].pair[0]`` or clearing ``rows[0].cells`` leaves the corresponding
fields in ``saved`` intact. A row returned by ``removeAt`` keeps its field
values after the original array is cleared. See the `struct cell example
<../../../Tests/Compiler/Driver/Inputs/growable-struct-cells.co>`_.

Assigning one growable array to another copies its elements. Appending
or changing a cell in either array leaves the other copy intact. A
fixed-size array can also initialize a growable one with the same
element type. Copying in the other direction requires the current
length to equal the fixed extent. When that length is known only at
runtime, a mismatch reports
``Cloth runtime error: array length does not match fixed extent``.

.. code-block:: text

   fn<int[]> takeRow(int[][] rows) {
       rows.append([1, 2]);
       rows.insert(0, [3, 4]);
       rows.reserve(8);
       def row = rows.removeAt(1);
       rows.clear();
       return row;
   }

``append(value)`` adds one element at the end. ``insert(index, value)``
places the value before the element at that ``usize`` index. Inserting
at ``values::size`` appends, so index zero is valid for an empty array.
``removeAt(index)`` requires an existing element, returns it, and shifts
the later elements left. An index beyond the insertion or removal bounds
reports ``Cloth runtime error: array index out of bounds``.

Mutation calls evaluate their arguments from left to right and use the
array's length after argument evaluation. ``clear()`` removes every
element. You can clear an empty array and insert or append again.
``reserve(count)`` requests capacity without changing the length. All
five operations require writable growable storage.

In the `insertion and removal example
<../../../Tests/Compiler/Driver/Inputs/growable-array-mutations.co>`_,
inserting 42 into ``[7, 9]`` at index one gives ``[7, 42, 9]``.
``removeAt(1)`` returns 42 and leaves ``[7, 9]``. The program clears the
array and appends the saved 42 to it.

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

For a growable array, the selected index survives a change in storage
during the right-hand side. If ``values[i] += expression`` reserves more
space or replaces ``values`` while evaluating ``expression``, it adds to
the saved cell value and writes at index ``i`` in the current array.
If the replacement is too short, the write reports an out-of-bounds
error. An indexed read uses the array value captured before evaluating
its index. See the `growable array updates
<../../../Tests/Compiler/Driver/Inputs/growable-array-values.co>`_.

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
``for`` loop visits, see `Array iteration
<../statements/ControlFlow.rst#array-iteration>`_.

For native array support and the byte distance between integer elements,
see `Fixed-size arrays in object files
<../../compiler/driver/Objects.rst#fixed-size-arrays>`_ and
`Integer storage layout <../../compiler/storage/Integers.rst>`_.
