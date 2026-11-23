.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Control flow
============

Conditionals
------------

An ``if`` tests a ``bool`` condition in parentheses. With one or more
``else if`` branches, Cloth tests the conditions in order and runs the
first branch whose condition is true. If all are false, the final
``else`` runs when present. A branch can be a single statement or a
braced block. A guard such as ``if (n == 5) break`` can omit ``else``.

When an ``if`` supplies a value, give it an ``else`` branch. The last
expression in the selected branch supplies its result. A ``return``
inside a branch leaves the enclosing function immediately, even when
the conditional is part of a larger expression.

.. code-block:: text

   fn<int> choose(bool flag) { return if (flag) 42 else 7 }

Only the selected branch is evaluated. A division in the other branch
is skipped, as in the `conditional expressions example
<../../../Tests/Compiler/Driver/Inputs/conditional-values.co>`_.

While and do/while loops
------------------------

A ``while`` loop tests its condition before each iteration. A ``do``
loop runs its body before the first test, then repeats while the
condition is true. Both forms take a ``bool`` condition in parentheses
and accept a single statement or a braced block.

.. code-block:: text

   int count = 0
   while (false) count++
   do { count++ } while (false)

The first loop leaves ``count`` at zero. The second increments it once.
A ``return`` from the body leaves the function immediately and skips the
next condition check. See the `while examples
<../../../Tests/Compiler/Driver/Inputs/while-loops.co>`_ and the
`do/while examples <../../../Tests/Compiler/Driver/Inputs/do-while-loops.co>`_.

Integer ranges
--------------

A ``for`` loop evaluates the start and end of its range once, from left
to right. ``start..end`` excludes the end and ``start..=end`` includes
it. The default step adds one. With reversed bounds the default loop
skips the body.

.. code-block:: text

   int total = 0
   for (int n <- 13..16) total += n

The loop adds 13, 14, and 15. Using ``13..=15`` gives the same sum, 42.
See the `half-open example <../../../Tests/Compiler/Driver/Inputs/integer-range-loop.co>`_
and the `inclusive example <../../../Tests/Compiler/Driver/Inputs/inclusive-integer-range-loop.co>`_.

Explicit steps
~~~~~~~~~~~~~~

After the bounds, a semicolon introduces an update of the loop counter,
as in ``for (int n <- 15..=13; n--)``. The body can be a single statement
or a braced block. The step runs after the body or on ``continue``, using
the counter value the body left behind. An empty range skips the step,
as does a ``break`` or ``return`` from the body.

Each example adds the values it visits and returns 42.

.. list-table::
   :header-rows: 1

   * - Example
     - Range and step
     - Values visited
   * - `Increment <../../../Tests/Compiler/Driver/Inputs/increment-integer-range-loop.co>`_
     - ``13..=15; n++``
     - 13, 14, 15
   * - `Decrement <../../../Tests/Compiler/Driver/Inputs/decrement-integer-range-loop.co>`_
     - ``15..=13; n--``
     - 15, 14, 13
   * - `Addition <../../../Tests/Compiler/Driver/Inputs/additive-integer-range-loop.co>`_
     - ``12..=16; n += 2``
     - 12, 14, 16
   * - `Subtraction <../../../Tests/Compiler/Driver/Inputs/subtractive-integer-range-loop.co>`_
     - ``16..=12; n -= 2``
     - 16, 14, 12
   * - `Multiplication <../../../Tests/Compiler/Driver/Inputs/multiplicative-integer-range-loop.co>`_
     - ``6..=24; n *= 2``
     - 6, 12, 24
   * - `Division <../../../Tests/Compiler/Driver/Inputs/division-integer-range-loop.co>`_
     - ``24..=6; n /= 2``
     - 24, 12, 6
   * - `Remainder <../../../Tests/Compiler/Driver/Inputs/remainder-integer-range-loop.co>`_
     - ``35..=7; n %= 14``
     - 35, 7 (the body breaks at 7)
   * - `Assignment <../../../Tests/Compiler/Driver/Inputs/assignment-integer-range-loop.co>`_
     - ``32..=2; n = n / 4``
     - 32, 8, 2

With ``n--``, the half-open range ``15..13`` visits 15 and 14. An
explicit step can also point away from the written end. If the body
leaves the counter alone, ``3..=1; n++`` visits 3 once and
``1..=3; n--`` visits 1 once.

For ``+=``, ``-=``, ``*=``, ``/=``, and ``%=``, the loop reads the counter,
evaluates the right operand once, and then applies the operation. For
addition and subtraction, a signed step can change direction between
iterations. Subtracting a negative step moves the counter upward.
Division truncates toward zero. A nonzero remainder has the sign of the
counter. Both operations raise an integer division error for a zero
divisor or the minimum signed value with divisor ``-1``.

With ``n = expression``, the loop evaluates the expression once and uses
its value as the next counter. Arithmetic in that expression follows the
ordinary integer rules. Assigning ``n + 1`` at the signed maximum wraps
to the signed minimum. The loop continues if that value is in range.

Stopping conditions
~~~~~~~~~~~~~~~~~~~

The loop exits when the new counter leaves the captured bounds. Increment,
decrement, addition, subtraction, and multiplication also end the loop if
the step would exceed the counter type's limits. With the counter unchanged
in the body, both the default step and ``n++`` visit ``usize::max..=usize::max``
once. A loop with ``n--`` visits ``usize::min..=usize::min`` once.

An update that leaves an in-range counter unchanged reports
``Cloth runtime error: range counter update made no progress``. In the
remainder example, taking one more step at 7 would produce 7 again.
Multiplication by zero can move a nonzero counter to zero. If zero is
in range, the body runs there and another multiplication leaves it unchanged.

Fixed-size array iteration
--------------------------

A ``for`` loop evaluates its fixed-size array source once and visits the
captured elements in index order. An empty array skips the body. Each
iteration binds a writable copy of its element. Changing the source
array inside the loop leaves the captured values intact. ``continue``
advances to the next element.

.. code-block:: text

   int[2] digits := [4, 2]
   int result = 0
   for (def digit <- digits) result = result * 10 + digit

The loop reads 4 and then 2, leaving ``result`` equal to 42. See the
`array iteration example <../../../Tests/Compiler/Driver/Inputs/fixed-array-iteration.co>`_.

Break and continue
------------------

``break`` exits the innermost loop whose body contains the jump.
Execution resumes after that loop, skipping any remaining step or
condition check. ``continue`` skips the rest of the current iteration.
In both ``while`` and ``do`` loops it goes straight to the condition.
A range loop runs its step, and a fixed-size array loop advances to the
next element.

Both jumps require an enclosing loop body. A jump in a nested loop's
condition or iteration source targets the surrounding loop. For example,
a ``break`` in the trailing condition of an inner ``do`` loop exits the
outer loop. See the examples of `jumps in loop conditions
<../../../Tests/Compiler/Driver/Inputs/loop-jumps.co>`_ and `jumps in array
iteration sources <../../../Tests/Compiler/Driver/Inputs/fixed-array-iteration-values.co>`_.

Scalar operators evaluate their operands in source order. If an operand
executes ``break``, ``continue``, ``return``, or the built-in ``exit``,
earlier side effects are preserved. Execution skips the remaining operands
and the enclosing operation. See the `operand examples
<../../../Tests/Compiler/Driver/Inputs/operand-exits.co>`_.
