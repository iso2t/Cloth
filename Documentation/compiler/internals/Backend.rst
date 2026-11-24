.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Lowering a checked program
==========================

For a program whose ``main`` calls an imported ``identity``, one object
contains both functions. ``main`` has external linkage and the helper has
internal linkage. The backend follows selected calls from the checked
entry point. Declaring each LLVM function before lowering its body lets
recursive calls use the same declaration. See the `object-file example
<../driver/Objects.rst#functions-in-another-file>`_.

Types and the target
--------------------

``LoweredModule`` creates an LLVM target machine using the toolchain's
triple. On x86 the requested width selects the architecture while retaining
the platform and ABI. Before lowering the entry point, the backend sets
the module's triple and data layout and checks LLVM's pointer width and
integer allocation and alignment against the checked ``TargetLayout``.
Target-dependent widths such as ``usize`` must agree at both ends of the
`frontend handoff <Frontend.rst#the-checked-program>`_.

``ValueTypes`` selects the representation for each use. For example,
a fixed-size array can live in a local, while the direct-call ABI takes
scalar parameters and nullable scalars. Struct fields follow the offsets
in ``CheckedProgram::structLayouts``.
A nullable scalar carries its presence information so a present zero
survives storage and a call. A fixed-size array local stores its elements
at the checked stride. See `LLVM value types
<../../../Backend/src/backend/value_types.cpp>`_, `integer layout
<../storage/Integers.rst>`_, and `aggregate layout <../storage/Aggregates.rst>`_.

``ValueTypes::localType`` builds one LLVM array per fixed dimension,
checking size and alignment against ``CheckedProgram::arrayDataLayout``.
For ``rows[r][c]``, lowering checks ``r`` against the row count before
evaluating ``c`` and checking it against the column count. A read from an
immutable local can address the element directly. If a subscript can
reassign a mutable local, lowering captures the receiver's value before
evaluating that subscript. The `rectangular array driver test
<../../../Tests/Compiler/Driver/rectangular-array.test>`_ exercises both
evaluation order and bounds failures.

For a nullable fixed-size array local, ``ValueTypes::localType`` wraps the
inline buffer with ``nullable::type``. Whole-local assignment copies the
presence byte and buffer together. When the frontend proves the local
present, ``tryArrayAddress`` uses ``nullable::payloadAddress`` to locate
the buffer for an indexed read or write. Array iteration and structural
comparison of present arrays use the same projection when borrowing a
slot. The `nullable array driver test
<../../../Tests/Compiler/Driver/nullable-fixed-array.test>`_ covers
copying, clearing the source during iteration, and indexed side effects.

For a built-in indexed write, ``lowerElementAssignment`` requests a
``MutableSlot`` from ``tryArrayAddress``. Each checked subscript addresses
an element within its parent buffer. Lowering keeps the final cell address
while it evaluates the right-hand side, even if that expression reassigns
the array local. A compound update loads the old cell before evaluating
its right-hand side. See the `nested assignment driver test
<../../../Tests/Compiler/Driver/rectangular-array-write.test>`_.

Control flow and effects
------------------------

Use the selected call and parameter bindings when lowering a call.
Evaluate explicit arguments in written order. When an operand transfers
control, preserve earlier effects and skip later operands and the operation
itself. For example, a ``return`` inside a call argument can finish the
caller before the callee is entered. At a branch join, give an LLVM phi
node an incoming value only from a block that actually reaches the join.

For a built-in ``RangeOperation::Construct``, lower both endpoints in
source order at the checked integer width. A stored range carries the start,
the end, and a one-byte inclusion flag, in that order. An immutable local
holds this aggregate in SSA. A value-producing ``if`` selects the whole
aggregate so the endpoints and flag come from the same arm. See the
`range representation <../../../Backend/src/backend/integer_range.cpp>`_ and
`checked storage layout <../../../Compiler/src/semantic/storage_layout.cpp>`_.

Locals and struct fields use the same integer range type from
``ValueTypes::get``. For ``range<T>?``, the standard nullable layout puts a
presence byte before the aligned range payload. Comparisons with ``null``
read that byte. The inclusion flag stays inside the payload, so even
the all-zero payload of ``0..0`` can be present. When the frontend
proves a local or field present, ``readValue`` projects that payload for
membership or iteration. The `nullable range driver test
<../../../Tests/Compiler/Driver/nullable-integer-range.test>`_ covers fallback
evaluation. The `struct range field test
<../../../Tests/Compiler/Driver/struct-range-fields.test>`_ checks that clearing
a field preserves the range in a previously copied struct.

The frontend records ``RangeOperation::Membership`` for a built-in
membership test. For ``is`` with a named value target, ``BodyFacts::valueTests``
holds the resolved binary expression. Lower its operands in source order,
including when equality places the range first. Compare the member with
the start and end at the checked width and signedness, and use the range's
inclusion flag at the upper bound. ``!=`` negates that predicate before
it becomes a Cloth boolean. The `membership driver test
<../../../Tests/Compiler/Driver/integer-range-membership.test>`_ verifies
endpoint effects, early returns, and both operand orders.

The frontend keeps the nullable range type when checking a conditional
operand, so a literal range and a ``null`` branch share the peer's type.
When it selects ``RangeOperation::Equal``, evaluate the left range before
the right. ``IntegerRange::equal`` compares each pair of
integer endpoints at its checked width and the inclusion flags as LLVM
predicates. For a comparison with a nullable operand, lift a plain range
to a present value. ``nullable::equal`` selects the structural result
when both operands are present and compares presence otherwise. This
keeps an unused payload out of the result. Negate the resulting predicate
for ``!=``, then convert it to a Cloth boolean. The `range equality
driver test <../../../Tests/Compiler/Driver/integer-range-equality.test>`_
and the `nullable range equality driver test
<../../../Tests/Compiler/Driver/nullable-integer-range-equality.test>`_
check presence, individual field differences, operand effects, and early
returns.

For built-in fixed-size array equality, the frontend checks rank and cell
type. ``tryLowerFixedArrayEquality`` compares the nested LLVM buffer types
to check every extent. It evaluates both operands once in source order.
When a computed right operand can reassign a mutable left local, lowering
captures the left presence and any cells the comparison needs.

For different shapes, equality holds only when both arrays are absent.
For matching empty shapes, the presence flags determine equality. Both
cases skip the cell loop. For matching nonempty shapes, a branch admits
only two present arrays to the loop. Its other edge returns the equality
of the flags, so an absent payload cannot affect control flow. The
`nullable comparison test
<../../../Tests/Compiler/Driver/nullable-fixed-array-equality.test>`_
exercises these paths and operand side effects.

Fixed rows occupy contiguous storage. Dividing the buffer's allocation
size by the cell's allocation stride gives the number of cells, including
for padded integer types. A single target-sized index visits the cells in
row order and stops at the first unequal pair. The loop uses scalar
equality or ``IntegerRange::equalPacked``, preserving the cell type's
rules for NaN, null, and range end inclusion. See the `rectangular
comparison test <../../../Tests/Compiler/Driver/rectangular-array-equality.test>`_
and the `range array test <../../../Tests/Compiler/Driver/fixed-array-range-equality.test>`_.

``FunctionLowering::lowerFor`` takes the counter type from
``BodyFacts::iterations``. A range written in the header supplies its
bounds directly. For a stored range, the loop extracts the bounds once
on entry. Both paths feed the same counter phi and use its checked
signedness and the inclusion flag for the end comparison. The
`stored range test <../../../Tests/Compiler/Driver/stored-integer-range-loop.test>`_
exercises repeated traversal and conditional bounds.

A range loop's ``continue`` goes to its step. For ``n = expression``, save
the counter the body left behind before evaluating the expression, which
can itself write to ``n``. The no-progress guard fires if the replacement
equals that saved value and would start another iteration. Use the block
after the guards as the predecessor for the phi node's next value. A step
that returns from the function contributes no back edge. The
`assignment driver test <../../../Tests/Compiler/Driver/assignment-integer-range-loop.test>`_
exercises the side effects, and the `lowering tests
<../../../Tests/Backend/Unit/lowering_test.cpp>`_ check the phi input.

For ``IterationKind::Array``, ``lowerFor`` captures the source with
``lowerArrayBuffer`` before entering the body. A mutable source needs a
copy because the body can change a row that the loop has yet to visit.
The target-sized index advances along the outer dimension. With inline
rows, each load yields a whole row, even when the row contains zero cells.
``bindLocal`` copies that value into a writable slot allocated once in the
function's entry block. Each visit initializes the slot from the captured
array. ``continue`` advances the index and ``break`` exits the loop.
The `array iteration driver test
<../../../Tests/Compiler/Driver/fixed-array-iteration.test>`_ checks row
mutation, empty dimensions, and nested traversal.

Runtime calls and object output
-------------------------------

Printing an integer calls ``cloth_rt_println_i32``. An integer division
with a variable divisor needs a failure path for zero. The runtime writes
the diagnostic and terminates the process. The `runtime dependencies
<../driver/Objects.rst#runtime-dependencies>`_ identify the C source a
caller needs when linking each operation.

The `backend <../../../Backend/src/backend/backend.cpp>`_ verifies the LLVM
module and asks the target machine to emit an object or assembly into
memory. The `driver's writer
<../../../Compiler/src/file/output_file.cpp>`_ checks that the requested
destination cannot overwrite an input source. It writes and closes an
exclusively created temporary file in the destination directory before
renaming it over the output. A failed check or write preserves a previous
output file. The `driver test <../../../Tests/Compiler/Driver/emit.test>`_
checks output names and assembles and runs the emitted code. The
`target tests <../../../Tests/Compiler/Driver/emit-object-targets.test>`_
inspect the object format, and the `failure tests
<../../../Tests/Compiler/Driver/emit-object-errors.test>`_ check diagnostics
and existing output files.
