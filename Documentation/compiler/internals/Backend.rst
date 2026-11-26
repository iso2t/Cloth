.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Lowering a checked program
==========================

For a program whose ``main`` calls an imported ``identity``, one object
contains both functions. ``main`` has external linkage and the helper has
internal linkage. The backend follows selected calls from the checked
entry point. Declaring each LLVM function before lowering its body lets
recursive calls use the same declaration. See the `object-file example
<../driver/Objects.rst#functions-in-another-file>`_.

Separate compilation
--------------------

The frontend records the logical names of library entry units in
``CheckedProgram::nativeModules``. An exported function is identified
by its declaring unit, its checked signature and its public visibility.
A named library seeds the lowering queue with its concrete public
top-level functions and their default argument thunks. Private helpers
and ordinary source imports enter the queue when reached. A call to
another native library creates an external LLVM declaration. Generic
instances continue to use the requesting object's specialization cache.

The ``cloth_v1`` link name encodes the normalized module and function
names as UTF-8 hex components. Parameter and result types
record the checked base name, integer or floating width and nullability.
A default thunk adds its parameter index. This makes the same overload
addressable after source relocation, declaration reordering or a
parameter rename. Changes
to that signature or to the calling convention require rebuilding
consumers. ABI changes to encoding or value representation require a new
version of the link name. The `separate-compilation regression
<../../../Tests/Compiler/Driver/native-library.test>`_ checks symbol
bindings and relinks an existing consumer object with a changed provider.

Types and the target
--------------------

``LoweredModule`` creates an LLVM target machine using the toolchain's
triple. On x86 the requested width selects the architecture while retaining
the platform and ABI. Before lowering the entry point, the backend sets
the module's triple and data layout and checks LLVM's pointer width and
integer allocation and alignment against the checked ``TargetLayout``.
Target-dependent widths such as ``usize`` must agree at both ends of the
`frontend handoff <Frontend.rst#the-checked-program>`_.

``ValueTypes`` selects the representation for each use. Internal direct
calls pass local value representations. A fixed-size array travels as a
snapshot of its cells. Struct fields in storage follow the offsets in
``CheckedProgram::structLayouts``.
A nullable scalar carries its presence information so a present zero
survives storage and a call. A fixed-size array local stores its elements
at the checked stride. See `LLVM value types
<../../../Backend/src/backend/value_types.cpp>`_, `integer layout
<../storage/Integers.rst>`_, and `aggregate layout <../storage/Aggregates.rst>`_.

A string value holds a data pointer and a target-sized UTF-8 byte length.
A literal's bytes live in a private constant global. Copying the string
copies the descriptor. For a checked ``ProviderKind::StringSize``, lowering
counts UTF-8 leading bytes up to the stored length, so an embedded zero
counts as a character. See `string lowering
<../../../Backend/src/backend/string_value.cpp>`_ and its `linked tests
<../../../Tests/Compiler/Driver/string-size.test>`_.

A fixed array field has two LLVM representations. An SSA struct value
holds the cells inline. A mutable struct slot uses the checked field
offsets and a descriptor pointing to a buffer allocated for that slot.
``ValueTypes::loadLocal`` takes a value snapshot and ``storeLocal`` writes
the cells into the destination's buffer. Whole-struct assignment therefore
preserves a cell address already captured for an indexed write. See
`aggregate loads and stores <../../../Backend/src/backend/aggregate_storage.cpp>`_,
`array storage <../storage/Aggregates.rst#array-storage>`_, and the
`linked field tests <../../../Tests/Compiler/Driver/fixed-array-fields.test>`_.

A nullable fixed-size array field has a presence byte for its current
value and a separate shape witness in the enclosing struct. Clearing
the field preserves the last known dimensions. Whole-struct copies
carry that history along with the current fields. A presence proof
lets an indexed write address the destination's backing buffer directly.
See `array storage <../storage/Aggregates.rst#array-storage>`_ for the
placement of the witness and the distinction between an absent field
and a present array with an empty dimension.

``ValueTypes::slotType`` describes a fixed array's addressable buffer.
The backend checks its size and alignment against
``CheckedProgram::arrayDataLayout``. A struct element occupies its checked
stride there, including its array field descriptors. In the SSA value
returned by ``localType``, that same element holds the field cells inline.
When the representations differ, ``copyBuffer`` traverses the dimensions
with counted loops and loads or stores each element's fields. A zero
extent skips the element traversal. At initialization, ``allocateBacking``
allocates a buffer for each nested array field and uses the surrounding
indices to give each struct element its own cells. See the `buffer copy
implementation <../../../Backend/src/backend/array_storage.cpp>`_ and
`array of structs tests <../../../Tests/Compiler/Driver/array-of-structs.test>`_.

For ``rows[r][c]``, lowering checks ``r`` against the row count before
evaluating ``c`` and checking it against the column count. A read from an
immutable local can address the element directly. If a subscript can
reassign a mutable local, lowering captures the receiver's value before
evaluating that subscript. The `rectangular array driver test
<../../../Tests/Compiler/Driver/rectangular-array.test>`_ exercises both
evaluation order and bounds failures.

For a nullable fixed-size array, both the physical slot and the SSA
value wrap their respective buffers with ``nullable::type``. Whole-local
assignment copies the presence byte and the cells. When the frontend
proves the local present, ``tryArrayAddress`` uses ``nullable::payloadAddress`` to locate
the buffer for an indexed read or write. Array iteration and structural
comparison of present arrays use the same projection when borrowing a
slot. The `nullable array driver test
<../../../Tests/Compiler/Driver/nullable-fixed-array.test>`_ covers
copying, clearing the source during iteration, and indexed side effects.

When the element is a nullable fixed-size row, the addressable element
contains a tagged descriptor. Its SSA value contains a tag and inline
cells. Indexed reads and writes use ``arrayElementPlace`` to select the
descriptor. Loading the row captures its cells, and storing it copies them
into the selected destination row's backing buffer. For the slot stride
and fixed shape table, see `array storage
<../storage/Aggregates.rst#array-storage>`_.

For a built-in indexed write, ``lowerElementAssignment`` asks
``tryArrayAddress`` for a writable address. Each checked subscript addresses
an element within its parent buffer. Lowering keeps the final cell address
while it evaluates the right-hand side, even if that expression reassigns
the array local. A compound update loads the old cell before evaluating
its right-hand side. See the `nested assignment driver test
<../../../Tests/Compiler/Driver/rectangular-array-write.test>`_.

.. _growable-integer-locals:
.. _growable-scalar-locals:

Growable array locals
---------------------

The checked ``ArrayMutation`` selects the runtime call for a growable
local. Lowering evaluates the receiver and each argument in source order.
``insert`` passes the captured value in a typed slot. ``removeAt`` supplies
an output slot of the element's LLVM type and loads it after the call.
Both pass a target-sized index and the checked cell stride and alignment.
The runtime checks the index against the current descriptor length, which
also supplies ``::size``. See the `linked mutation tests
<../../../Tests/Compiler/Driver/growable-array-mutations.test>`_.

Copying to fixed-size storage also checks the current length against the
destination extent. See the `runtime call builders
<../../../Backend/src/backend/growable_array.cpp>`_ and `growable buffer
layout <../storage/Aggregates.rst#growable-array-buffers>`_.

Indexing, removal, and iteration take the cell type from
``ValueTypes::localType``. A nullable cell carries its presence byte
through these operations. An indexed read extracts the payload when
the frontend has proved presence. Before issuing a buffer operation,
``arrayCellLayout`` checks LLVM's allocation size and alignment against
``CheckedProgram::storageLayout``. See the `nullable cell tests
<../../../Tests/Compiler/Driver/growable-nullable-cells.test>`_.

An indexed read captures a mutable array before an index expression can
replace it. An indexed write saves its coordinate and checks bounds
before the right-hand side. A compound update also saves the old cell
value. The store uses the current descriptor and length because either
can change while evaluating the right-hand side.

``ArrayLifetimes`` tracks local owners and temporary snapshots in
zero-initialized entry-block slots. Temporaries are released after the
statement consumes them. Exiting a block releases its locals, including
when a ``break``, ``continue``, or ``return`` crosses that block.
The `lifetime test <../../../Tests/Compiler/Driver/Inputs/growable-array-lifetime.co>`_
checks the live allocation count at scope boundaries. See `lifetime lowering
<../../../Backend/src/backend/array_lifetimes.cpp>`_ and the `linked driver test
<../../../Tests/Compiler/Driver/growable-array.test>`_.

Class reference storage
-----------------------

Class and interface values use an LLVM 64-bit integer on both targets. An array copy
therefore retains each instance's identity while its destination gets
independent cells. Raw ``===`` compares handles and accounts for nullable
operands. Array equality consults the checked element operator selection.

Construction evaluates the checked arguments and defaults, allocates one
instance, and passes its handle as a hidden parameter through constructor
delegation. The concrete base occupies the prefix of the instance's
storage. Each class's remaining fields follow their checked
``classFieldIndex`` order. Lowering checks the combined field sizes and
alignment padding against the target's addressable size before emitting
the allocation. Fixed array backing storage inside a stored
aggregate belongs to the instance. Growable fields own their buffers.
See `class layout and callbacks <../../../Backend/src/backend/class_objects.cpp>`_.

The runtime validates the slot and generation before exposing its data.
For a direct method call, the backend captures the receiver and applies
the checked ``beforeArguments`` and ``atAccess`` facts around argument
evaluation. A field write retains its receiver through evaluation of
the source expression. Deletion marks the instance as being destroyed,
invokes the concrete destructor chain, releases native field storage,
and retires the handle. Direct destructor access to ``self`` is permitted
during that chain. See the `runtime ABI <../../../Runtime/include/cloth/object.h>`_
and `linked lifetime tests <../../../Tests/Compiler/Driver/class-lifetime.test>`_.

Method and state dispatch
-------------------------

The checked call records a static receiver, a contract, and any concrete
method type arguments. Lowering assigns that combination a slot. As
reachable bodies allocate classes or introduce more slots, the backend
resolves each new class/slot pair against the frontend's ``TypeDispatch``.
It queues the selected implementations and publishes the constant tables
once the body queue is empty. The call evaluates arguments and defaults
using its static contract before looking up the implementation on the
captured handle. See the `dispatch emitter <../../../Backend/src/backend/dispatch.cpp>`_.

Each object records its concrete descriptor and active state. A state row
contains the callable slots provided by that state. A separate state
definition attached to two hosts can require two bodies, each using its
host's checked attachment facts. The runtime replaces the previous payload
on transition and releases it on deletion. A running method holds a
snapshot when another transition replaces that payload. See the
`object runtime <../../../Runtime/src/object.c>`_.

Reference comparisons through a base class or interface use the checked
equality candidates for the concrete receiver and test the peer's nominal
type before calling a body. The frontend orders candidates with the same
specificity rule it uses for a direct operator and records competing peer
types. A match against a competing type reports an ambiguous overload.
Structural array comparisons use the selected
element operator, preserve operand order, and stop on the first mismatch.
For a checked indexed write, lowering captures the receiver and index once
and invokes the selected validator before the read and before the store.
A dedicated update on a class element needs only the captured handle after
the getter. See the `linked operator tests <../../../Tests/Compiler/Driver/operator-dispatch.test>`_.

Direct calls and results
------------------------

``ModuleFunctions`` caches each LLVM declaration before queuing its body.
The key contains the declaration, the frontend's canonical closed
``InstanceKey`` when specialized, and the parameter index for a default.
Recursive calls and calls from other modules reuse the declaration.
A different concrete receiver, type argument, or operator selector can
require another body. Only reachable instances enter the emission queue.

Each closed callable uses its ``GenericInstance`` signature and body facts
for names, expressions, control flow, and nested calls. The enclosing
type instance supplies field types, base types, and primary constructor
initializer facts. Constructor guarantees store a separate checked
signature on the enclosing instance so their defaults can reference
the guarantee's own parameters. See the `frontend specialization records
<../../../Compiler/include/semantic/checked_program.hpp>`_.

``argumentContract`` identifies the declaration
that owns the defaults. Lowering evaluates the supplied expressions in
source order and places their values using ``parameterForArgument``.
It then visits ``omittedParameters`` in order. Each default is an internal
function receiving the preceding parameter values. Its body uses the
declaring scope, and recursive calls reuse the same LLVM declaration.
See the `function ABI implementation <../../../Backend/src/backend/function_abi.cpp>`_.

A direct struct method receives the physical address of its receiver,
the handle of any class that owns that storage, and the receiver's
destructor-access permission. A mutable place lends its address for the
call. A value expression supplies a temporary copy. The callee validates
a borrowed owner before accessing storage, including after an argument or
right-hand side can delete that owner. The same receiver ABI applies to
defaults and directly selected operators. See the `linked specialization
and borrowed-receiver tests <../../../Tests/Compiler/Driver/generic-specialization.test>`_.

Zero results use LLVM ``void``, one uses its value type, and several use
an LLVM struct in declaration order. A grouped write captures the source
values before changing any destination. Growable results transfer
independent buffers to the caller, which registers each for cleanup.
For a deferred read, lowering uses the checked block and result index.
Each return in that block branches to the read's continuation after
preserving the results and releasing block-local owners. The enclosing
function's locals remain available on the next read. See the
`linked call and result tests <../../../Tests/Compiler/Driver/call-results.test>`_.

The external C ``main`` adapts host arguments and exit status when the
Cloth signature requires it. Calls from Cloth use the original
signature. The runtime keeps argument bytes alive until the entry
returns, including after a Cloth array copy or resize. See the
`argument runtime <../../../Runtime/src/arguments.c>`_ and
`process status cases <../../../Tests/Compiler/Driver/entry-arguments.test>`_.

Control flow and effects
------------------------

Use the selected call and parameter bindings when lowering a call.
Evaluate explicit arguments in written order. When an operand transfers
control, preserve earlier effects and skip later operands and the operation
itself. For example, a ``return`` inside a call argument can finish the
caller before the callee is entered. At a branch join, give an LLVM phi
node an incoming value only from a block that actually reaches the join.

``lowerGuardedAccess`` evaluates the receiver once. When the checked
type is nullable, its presence byte branches around the continuation.
On the present edge, the ``GuardedReceiver`` node refers to the captured
payload, so a nested field read uses the ordinary checked struct layout.
A frontend presence proof supplies that payload directly. A plain field
result becomes present, and an already nullable field keeps its own tag.
The absent edge supplies a null result to the join. The `guarded field
test <../../../Tests/Compiler/Driver/guarded-struct-field.test>`_ covers
nested guards, fallback side effects, and a receiver that returns early.

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

For built-in array equality, the frontend checks rank and cell type.
``tryLowerArrayEquality`` compares nested LLVM buffer types when both
shapes are fixed. It evaluates both operands once in source order.
When a computed right operand can reassign a mutable left local, lowering
captures the left presence and any cells the comparison needs.

For a growable operand, ``lowerArrayBuffer`` reads the
descriptor's logical length. A fixed-size peer supplies its declared
length. ``equalArrayBuffers`` enters the cell loop only for two present
arrays with equal, nonzero lengths. It loads each cell at its checked
type. Scalar comparison uses the full integer width, the boolean truth
value, the Unicode scalar value, or ordered floating-point equality.
Even a comparison of a buffer with itself visits the cells because a
NaN compares unequal to itself. The `growable scalar tests
<../../../Tests/Compiler/Driver/growable-scalar-arrays.test>`_ exercise
signed zero and NaN at all three floating-point widths.
The `linked traversal tests
<../../../Tests/Compiler/Driver/growable-array-traversal.test>`_
exercise mixed storage, empty buffers, and operand evaluation order.

For different shapes, equality holds only when both arrays are absent.
For matching empty shapes, the presence flags determine equality. Both
cases skip the cell loop. For matching nonempty shapes, a branch admits
only two present arrays to the loop. Its other edge returns the equality
of the flags, so an absent payload cannot affect control flow. The
`nullable comparison test
<../../../Tests/Compiler/Driver/nullable-fixed-array-equality.test>`_
exercises these paths and operand side effects.

Contiguous rows that are always present share a flat cell buffer.
Dividing its allocation size by the cell's stride gives the cell count, including
for padded integer types. A single target-sized index visits the cells in
row order and stops at the first unequal pair. The loop uses scalar
equality or ``IntegerRange::equalPacked``, preserving the cell type's
rules for NaN, null, and range end inclusion. See the `rectangular
comparison test <../../../Tests/Compiler/Driver/rectangular-array-equality.test>`_
and the `range array test <../../../Tests/Compiler/Driver/fixed-array-range-equality.test>`_.

``FunctionLowering::equalArrayBuffers`` walks contiguous dimensions in one
loop up to the next nullable row. Two absent rows compare equal.
A difference in presence ends the comparison. When both rows are
present, it recurses into their elements. An empty row still
has a presence tag to compare. The `nullable row driver test
<../../../Tests/Compiler/Driver/nullable-array-rows.test>`_ covers nested
row tags, equality after clearing or replacing a row, and a right
operand that reassigns the left array.

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
For a growable source, the entry branch skips an empty buffer
and the back edge uses the captured length. A mutable source supplies
an owned snapshot for the lifetime of the loop. A read-only local can
lend its buffer directly.
``bindLocal`` copies that value into a writable slot allocated once in the
function's entry block. Each visit initializes the slot from the captured
array. ``continue`` advances the index and ``break`` exits the loop.
The `array iteration driver test
<../../../Tests/Compiler/Driver/fixed-array-iteration.test>`_ checks row
mutation, empty dimensions, and nested traversal.

Standard library calls and stored values
----------------------------------------

A collection loop consumes the frontend's ``Iteration`` record. Lowering
calls its selected ``foreach`` operator once and projects the checked slots
and element fields. A returned snapshot owns its growable buffers through
the loop exit. Assigning a struct or fixed array containing those buffers
copies the buffers before releasing the destination. Class disposal,
temporary cleanup and return-value transfers use the same per-type
operations. See `buffer ownership <../../../Backend/src/backend/buffer_ownership.cpp>`_.

``BuiltinCall`` supplies the primitive operation and its adapted arguments.
``ProviderSelection`` and ``BodyFacts::textConversions`` supply formatters
for explicit conversions, printing, and interpolation. A type information
value points to a module constant with its spelling, element descriptor,
rank, extent, width and predicates. See `built-in lowering
<../../../Backend/src/backend/builtin_lowering.cpp>`_ and `type metadata
<../../../Backend/src/backend/type_metadata.cpp>`_.

A string carries a byte pointer and a target-sized length. The text
runtime interns dynamically produced UTF-8 and releases the stored bytes
at process exit. Copies and substrings can share immutable bytes. Programs
that generate many distinct strings retain that storage for the life of
the process. Runtime calls receive pointers to aggregates so both x86
targets use the host C calling convention.

The ``DeclarationInfo::intrinsic`` selection identifies a registered file
or decoder operation. A file handle belongs to its object registry slot.
The lowering maps runtime status codes to the checked enum cases by name,
so the library can reorder its cases. See the `file runtime
<../../../Runtime/src/files.c>`_ and the `linked consumers
<../../../Tests/Compiler/Driver/stdlib-native.test>`_.

Runtime calls and object output
-------------------------------

Printing a signed 32-bit integer with ``println`` calls ``cloth_rt_println_i32``. An integer division
with a variable divisor needs a failure path for zero. The runtime writes
the diagnostic and terminates the process. The `runtime dependencies
<../driver/Objects.rst#runtime-dependencies>`_ identify the C source a
caller needs when linking each operation.

For the checked two-argument ``exit``, lowering evaluates the status and
message in source order. It passes the converted 32-bit status, data
pointer and target-sized byte length to ``cloth_rt_exit_message``. The
`process runtime <../../../Runtime/src/process.c>`_ writes exactly that
span, including embedded zeros. Both exit helpers are non-returning.
The `linked exit tests <../../../Tests/Compiler/Driver/process-exit.test>`_
check observable argument effects, output bytes and process status.

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
