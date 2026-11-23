.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Operators
=========

In ``2 + 3 * 4`` the multiplication is evaluated before the addition,
giving 14. Parentheses change the grouping, so ``(2 + 3) * 4`` gives 20.
Exponentiation groups from the right. ``2 ** 3 ** 2`` means
``2 ** (3 ** 2)``.

Grouping
--------

From weakest to strongest, the binary operators group as follows.

#. ``?:`` (nullable fallback)
#. ``||``
#. ``&&``
#. ``==``, ``!=``, and ``===``
#. ``<``, ``<=``, ``>``, ``>=``, and ``is``
#. ``..`` and ``..=``
#. ``->`` (conversion)
#. ``+`` and ``-``
#. ``*``, ``/``, and ``%``
#. ``**``

Fallback and exponentiation associate to the right. A leading numeric
sign applies after exponentiation, so ``-2 ** 2`` gives -4.
``!`` negates a boolean. Parenthesize the part you want to evaluate first.

Ranges and membership
---------------------

``low .. high`` excludes the ending value. ``low ..= high`` includes it.
A range is a value of type ``range<T>``, with a common non-null numeric
type for the endpoints. A typed endpoint can supply the width of a
literal peer. Floating-point endpoints must be finite.

.. code-block:: text

   fn<bool> within(int<8> n) {
       range<int<8>> small = 1 .. 4
       return n is small
   }

This test includes 1, 2, and 3. Comparing a number and a range with
``==`` also tests membership, in either order. ``!=`` negates the test.
Two ranges can be compared when their endpoint types match. For the
order in which a loop visits the values, see `Integer ranges
<../statements/ControlFlow.rst#integer-ranges>`_.

Integer arithmetic
------------------

Addition, subtraction, multiplication, and exponentiation wrap at the
operand width. Adding 1 to an ``int<12>`` with value 2047 produces
-2048 on both target widths. Unary ``~`` flips every bit at that width.
Integer comparisons use the signedness and width of the checked operand
type. See the `integer examples
<../../../Tests/Compiler/Driver/Inputs/primitive-integers.co>`_.

Signed division truncates toward zero. A nonzero remainder has the
sign of the dividend, so ``-7 / 3`` gives -2 and ``-7 % 3`` gives -1.
The operators ``/``, ``%``, ``/=``, and ``%=`` evaluate both operands
once in source order.

The compiler diagnoses these integer errors when it recognizes them in
literal operands. An arithmetic or conversion error at runtime flushes
standard output, writes one diagnostic to standard error, and ends the
program with status 1.

.. list-table::
   :header-rows: 1

   * - Operation
     - Literal diagnostic
     - Runtime message
   * - Integer division or remainder by zero
     - ``C3050``
     - ``Cloth runtime error: integer division by zero``
   * - Signed minimum divided by -1, including remainder
     - ``C3051``
     - ``Cloth runtime error: signed integer division overflow``
   * - Integer power with a negative exponent
     - ``C3018``
     - ``Cloth runtime error: negative integer exponent``

Numeric conversions
-------------------

Integer arithmetic on two typed values requires matching operand types.
Choose the conversion explicitly when the widths or signedness differ.
Narrowing an integer keeps the low bits. Widening a signed integer
extends its sign, and widening an unsigned integer fills with zeros.
For example, casting an ``int`` variable holding 255 to ``int<8>`` gives -1.

A cast such as ``x -> int<8>`` uses the width of the destination. A
floating-point operand is truncated toward zero before the integer
range check. Converting ``-128.75`` to ``int<8>`` gives -128.
Converting ``-129.0`` to the same type reports
``Cloth runtime error: invalid float-to-integer cast (NaN or out of range)``.
The same error applies to NaN and either infinity. See the
`float-to-integer examples <../../../Tests/Compiler/Driver/float-to-integer.test>`_.

Floating-point comparisons use the stored precision. Positive and
negative zero compare equal. Ordered comparisons with NaN are false,
and NaN compares unequal to itself. The same rules apply to the
payloads of two present nullable floats.

Operator overloads and equality
-------------------------------

A user-defined type can supply an operator for its values. Cloth uses the
applicable overload in preference to the built-in operation. Operators are
private by default. Write ``pub operator`` when callers outside the declaring
type should be able to use one.

.. code-block:: text

   typedef<struct> Position {
       int offset;
       pub operator ==(Position other) = offset == other.offset
       pub operator +(int step) -> Position = { offset + step }
   }

The receiver normally appears on the left. A final ``self`` in the parameter
list places it on the right. Operands are required parameters. An exact
match takes precedence over a compatible one, and equally specific matches
are ambiguous. A class may declare ``==(Item)`` for a particular operand
type, ``==(Object)`` for a general operand, or both.

An operand context such as ``operator<string> ==()`` supplies a value for
comparison with a string. Cloth first looks for a full binary overload.
When it applies contexts, it evaluates each operand and its selected
context in source order.

An integer context can use ``int<auto>`` to take its width from the other
operand. For example, ``operator<int<auto>> +() -> int<auto>`` can supply
a value at the width requested by a typed integer peer. See the
`operator context tests <../../../Tests/Compiler/Semantic/Unit/operator_test.cpp>`_.

Evaluation and updates
----------------------

Built-in ``a && b`` evaluates ``b`` only if ``a`` is true. Built-in
``a || b`` evaluates it only if ``a`` is false. In ``a ?: b``, the
fallback runs only when ``a`` is absent. A present ``false``, ``0.0``, or
``'\0'`` skips the fallback.

An explicit binary overload evaluates both operands in source order,
including overloads of ``&&``, ``||``, and ``?:``. A dedicated compound
update such as ``+=`` takes precedence over applying the arithmetic
operator and assigning its result. Updates return ``void``.

To support ``items[i]``, declare ``operator [](Index i) -> T``. For writes,
declare ``operator []=(Index i, T value)`` and a bounds validator
``operator []?(Index i) -> bool`` on the setter's declaring type. The
validator can be private and must be read-only. The write captures the
receiver and index, checks the index before evaluating the new value,
and checks it again before storing. See the `List implementation
<../../../Stdlib/cloth/collections/List.co>`_.

A compound update on a user-defined collection requires a getter and
setter that share the same bounds validator. A dedicated update on a
class element captures the handle returned by the getter and applies
the update to that object.
For built-in arrays, see `Indexing and updates
<../types/Arrays.rst#indexing-and-updates>`_.

Use a target selector for a conversion or a type test. For example,
``operator<int> ->()`` supplies a conversion to ``int``, and
``operator<int> is()`` supplies a boolean test for that target.
In ``item is Gauge`` the target is a type. For a class's named states the
same syntax tests the active state, as in `State tests
<../objects/States.rst>`_.

Default equality
----------------

Converting an integer to ``bool`` produces
``false`` (0) for zero and ``true`` (1) for every positive or negative
nonzero value. A boolean binding, parameter, or return value accepts this
conversion. For example, ``bool ready = 0`` stores ``false``. A cast such as
``count -> bool`` makes the conversion explicit in a condition.

Built-in ``==`` and ``!=`` between a boolean and an integer convert the
integer to ``bool`` on either side. Thus ``false == 1`` is false,
``true == -2`` is true, and ``0 == false`` is true. A nullable value
preserves its absence through conversion.

Class and interface references support logical comparison and identity.
Struct values require an applicable equality operator. Enums have a built-in
equality operation. For ``!=``, an explicit inequality overload takes
precedence, followed by negating an applicable ``==``. Two absent nullable
values compare equal. If only one is present, they compare unequal. Two
present values use their payloads' equality rule, including any overload.
The built-in identity operator ``===`` compares class and interface references.

Array equality
--------------

Arrays compare structurally with ``==`` and ``!=``. The array rank, internal
nullable positions, and element type must match. Declared extents may
differ. Array inequality negates the structural equality result, so a
struct element must supply ``==`` even when the caller writes ``!=``.

Both operands are evaluated once, from left to right, even when their lengths
differ. Arrays of different lengths compare unequal. Two empty arrays with
the same element type compare equal. For equal lengths, comparison stops at
the first unequal pair.

For nullable elements, two absent values compare equal. If only one element
is present, the pair compares unequal. When both are present, their payloads
use the element type's equality rule. In the `nullable integer array example
<../../../Tests/Compiler/Driver/Inputs/fixed-array-nullable-equality.co>`_,
``[0, null]`` equals its copy. Swapping the positions of zero and null
makes the arrays unequal.

Integer elements compare at their declared width. In the `65-bit example
<../../../Tests/Compiler/Driver/Inputs/fixed-array-equality.co>`_, a copy
initially compares equal. Setting the highest bit in the original's second
element makes the arrays unequal, and the program returns 42.

Boolean elements compare by truth value, characters by Unicode scalar value,
and enums with built-in equality by case tag. Distinct enum cases can carry
identical payloads and still compare unequal.

Floating-point elements compare at their stored precision. Positive and
negative zero compare equal at 16, 32, and 64 bits. An array containing NaN
compares unequal even to itself, as the `floating-point array example
<../../../Tests/Compiler/Driver/Inputs/fixed-array-scalar-equality.co>`_ shows.
