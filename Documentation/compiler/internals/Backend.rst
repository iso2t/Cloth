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

Control flow and effects
------------------------

Use the selected call and parameter bindings when lowering a call.
Evaluate explicit arguments in written order. When an operand transfers
control, preserve earlier effects and skip later operands and the operation
itself. For example, a ``return`` inside a call argument can finish the
caller before the callee is entered. At a branch join, give an LLVM phi
node an incoming value only from a block that actually reaches the join.

A range loop's ``continue`` goes to its step. An array loop's ``continue``
advances through the captured array. A ``break`` skips both. Capturing the
source once preserves the elements the loop visits if its body assigns
to the original local. The `lowering tests
<../../../Tests/Backend/Unit/lowering_test.cpp>`_ exercise these transfers
and the resulting IR.

Runtime calls and object output
-------------------------------

Printing an integer calls ``cloth_rt_println_i32``. An integer division
with a variable divisor needs a failure path for zero. The runtime writes
the diagnostic and terminates the process. The `runtime dependencies
<../driver/Objects.rst#runtime-dependencies>`_ identify the C source a
caller needs when linking each operation.

The `backend <../../../Backend/src/backend/backend.cpp>`_ verifies the LLVM
module and emits object bytes into memory. The `driver's writer
<../../../Compiler/src/file/object_file.cpp>`_ checks that the requested
destination cannot overwrite an input source. It writes and closes an
exclusively created temporary file in the destination directory before
renaming it over the output. A failed check or write preserves a previous
object. The `target tests <../../../Tests/Compiler/Driver/emit-object-targets.test>`_
inspect the emitted format, and the `failure tests
<../../../Tests/Compiler/Driver/emit-object-errors.test>`_ check diagnostics
and existing output files.
