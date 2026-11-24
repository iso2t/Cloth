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

For fixed-size arrays of integer ranges, the array comparison loop passes
each pair of loaded elements to ``IntegerRange::equalPacked``. This uses the
same endpoint, inclusion, and nullable presence rules as standalone range
equality. The loop stops at the first unequal pair. The `range array
driver test <../../../Tests/Compiler/Driver/fixed-array-range-equality.test>`_
covers a right operand that mutates the left array and differences at a
single index.

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

An array loop's ``continue`` advances through the captured array. A
``break`` skips the range step or array advance. Capturing the array
source once preserves the elements visited if the body assigns to the
original local.

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
