.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Operator overloads and equality
===============================

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

Evaluation and updates
----------------------

An explicit binary overload evaluates its two operands in source order,
including overloads of ``&&``, ``||``, and ``?:``. The built-in forms evaluate
the right operand only when its value is needed. A dedicated compound update
such as ``+=`` takes precedence over applying the arithmetic operator and
assigning its result. Updates return ``void``.

Updating an indexed element requires a getter and setter that share the
same bounds validator. A dedicated update on a class element captures the
handle returned by the getter and applies the update to that object.

Use a target selector for a conversion or a type test. For example,
``operator<int> ->()`` supplies a conversion to ``int``, and
``operator<int> is()`` supplies a boolean test for that target.

Default equality
----------------

A ``bool`` occupies one byte. Converting an integer to ``bool`` produces
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
precedence, followed by negating an applicable ``==``. Comparing nullable
values checks presence before calling an overload on their payloads.

Arrays compare structurally with ``==`` and ``!=``. The array rank, internal
nullable positions, and element type must match. Declared extents may
differ. Array inequality negates the structural equality result, so a
struct element must supply ``==`` even when the caller writes ``!=``.
The built-in identity operator ``===`` compares class and interface
references.
