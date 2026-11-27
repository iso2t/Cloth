.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Aggregate storage layout
========================

Struct fields, nullable payloads, and array elements each have an
alignment as well as a byte size. The examples below use the 32-bit and
64-bit x86 targets.

Struct fields
-------------

Fields appear in declaration order. Each begins at the next offset that
meets its alignment. The struct's size is rounded up to a multiple of
its largest field alignment. For fields of types ``int<8>``, ``int<12>``,
and ``int<8>``, the offsets are 0, 2, and 4. The struct occupies 6 bytes.

A nested struct is stored inline. In ``Sample``, the ``Inner`` field
occupies 4 bytes with 2-byte alignment and starts at byte 2. The offsets
below are measured from the beginning of ``Sample``. See the
`nested struct declaration <../../../Tests/Compiler/Driver/Inputs/nested-struct.co>`_.

.. list-table::
   :header-rows: 1

   * - Field
     - Type
     - Byte offset
   * - ``tagged``
     - ``bool``
     - 0
   * - ``inner.ready``
     - ``bool``
     - 2
   * - ``inner.value``
     - ``int<12>``
     - 4

``Sample`` occupies 6 bytes with 2-byte alignment.

A numeric range field stores the start, end, and one-byte end-inclusion
flag inline. In a ``range<int<12>>`` field these start at offsets 0, 2,
and 4 within the field. Its size is 6 bytes with 2-byte alignment.
In a ``range<int<12>>?`` field, a separate presence byte occupies
offset 0 and the range starts at offset 2, giving a total of 8 bytes.

Enum tags and nullable values
-----------------------------

An enum value stores a four-byte tag. A payload field read uses that
tag to select the field value for the case. A nullable value stores a
one-byte presence flag at offset zero, followed by padding as needed
for the payload's normal alignment. In a nullable enum, the presence
byte distinguishes an absent value from the first case, whose tag is zero.

.. list-table::
   :header-rows: 1

   * - Type
     - Payload offset in bytes
     - Total size in bytes
   * - ``bool?``
     - 1
     - 2
   * - ``int?``, ``float?``, ``char?``
     - 4
     - 8
   * - Nullable enum
     - 4
     - 8

A ``bool`` occupies one byte. A ``char`` uses four bytes to store a
Unicode scalar value, including U+0000. A nullable character keeps all
four payload bytes. For the source rules, see `Nullable values <../../language/types/Types.rst#nullable-values>`_.

Array storage
-------------

In the compiler's layout for struct fields, a one-dimensional array has
a descriptor containing its data pointer, length, capacity, and owner.
Those four words occupy 16 bytes on a 32-bit target and 32 bytes on a
64-bit target. A nullable array field reserves additional space after
the declared fields to remember its shape when its value is cleared.

The descriptor puts the data pointer and outer length first. For each
inner dimension, a target-sized count and a one-bit knownness flag occupy
two aligned words. Arrays with nullable rows also carry a shared shape
pointer. Capacity and owner follow the shape fields. For a fixed
``int<12>[2][0]`` field, the outer length and capacity are both two, and
the inner count is a known zero. Its descriptor occupies 24 or 48 bytes
on the two targets. See the `descriptor layout tests
<../../../Tests/Backend/Unit/array_descriptor_test.cpp>`_.

In ``int[2]?[2]``, the outer buffer stores two nullable row slots. Each
slot starts with a presence byte, followed by padding and the row's
four-word descriptor. Row slots are 20 bytes apart on a 32-bit target
and 40 bytes apart on a 64-bit target. Each descriptor addresses its
own two integer cells. Clearing a row changes its presence, and assigning
a new pair reuses the slot's backing storage.

The outer descriptor's shared shape pointer refers to an immutable
table with one count and knownness flag per dimension. Every fixed
extent is known, including zero. Copies can share that table while each
destination retains its own row slots and cells. See the `nullable row
tests <../../../Tests/Compiler/Driver/nullable-array-rows.test>`_.

An addressable struct local has a separate stack buffer for each
supported fixed-size array field. Its descriptor points to that buffer
and has a null owner. Reading the struct captures its cells by value.
Assignment copies those cells into the destination's existing buffers,
so a selected cell keeps its address across reassignment of the enclosing
struct. The `field copy test
<../../../Tests/Compiler/Driver/fixed-array-fields.test>`_ exercises
copies in loops, nested fields, and a nullable parent.

When a struct also contains a growable field, the backend captures the
fixed cells and clones the growable buffers before releasing the
destination's old buffers. It then writes the fixed cells into the
destination's existing backing storage. Cleanup walks the stored fields
using their physical offsets, including any owned buffers in fixed arrays
of structs. Function arguments and results use the inline value layout.
The `mixed field copy test <../../../Tests/Compiler/Driver/mixed-array-fields.test>`_
checks both copy independence and allocation balance.

Each nullable array field adds a hidden shape witness after the declared
fields, in declaration order. It has the same storage type as the field.
Its tag starts at zero when the field has never held an array. The first
present array establishes the shape, including any known zero extents.
Clearing the field preserves the witness. Its data and owner pointers are
null and its capacity is zero, so the remembered shape holds no allocation.

For fixed extents the LLVM value copy carries one extra byte per witness.
That byte records whether a shape has been established. On assignment to
a struct, the backend rebuilds each physical witness from the copied tag
and the declared extents. Replacing the whole struct therefore also
replaces its shape history. See the `nullable field regression
<../../../Tests/Compiler/Driver/nullable-fixed-array-fields.test>`_ and
the `witness layout test <../../../Tests/Backend/Unit/array_descriptor_test.cpp>`_.

For growable fields the witness carries the last present outer length
and the inner extents, including which extents are known. A struct copy
copies this shape and gives the destination its own array buffers.
Assigning ``null`` to the field preserves the established inner extents.
Assigning a whole struct replaces them with the source's extents. See
the `growable field regression
<../../../Tests/Compiler/Driver/nullable-growable-array-fields.test>`_.

The element buffer occupies the element count times the allocation size
of one element. For integer elements, see `Integer storage layout <Integers.rst>`_.

In an array of structs, the stride is the size of the stored struct,
including its field descriptors and any shape witnesses. For a ``Row``
with one ``int[2]`` field, consecutive rows start 16 bytes apart on a
32-bit target and 32 bytes apart on a 64-bit target. Each row's
descriptor points to its own eight bytes of integer cells. See the
`array of structs example <../../../Tests/Compiler/Driver/Inputs/array-of-structs.co>`_.

A rectangular fixed-size local stores its rows inline. In
``int<12>[2][3]``, each integer occupies two bytes, so the second row
starts at byte 6 and the whole buffer occupies 12 bytes with two-byte
alignment. A fixed row of length zero occupies zero bytes and retains
the alignment of its element type.

A nullable fixed-size array local has a presence byte at offset zero,
followed by padding to the inline buffer's alignment. For ``usize[2]?``,
the buffer begins at byte 4 on a 32-bit target and byte 8 on a 64-bit
target. The local occupies 12 and 24 bytes respectively. An empty buffer
retains its alignment and the local still stores the presence byte.

.. _growable-integer-buffers:
.. _growable-scalar-buffers:

Growable array buffers
----------------------

A one-dimensional growable scalar local stores a four-word descriptor
with a data pointer, current length, capacity, and allocation owner.
Length and capacity count elements. The data pointer satisfies the
element alignment, and the owner points to the allocation the runtime
must free. A fresh empty descriptor has all four words zero. An empty
array with reserved storage can have a nonzero capacity.

In a growable array of structs, each live cell uses the checked struct
layout. If a struct contains array fields, the compiler supplies copy
and disposal functions for its cell type. A copy gets its own backing
for fixed fields and independent buffers for growable fields. A nullable
field also retains its shape witness. Reserving capacity or shifting cells
moves their existing owners. Clearing or releasing the array disposes
each live cell. The `struct cell regression
<../../../Tests/Compiler/Driver/growable-struct-cells.test>`_ also checks
that removing a cell preserves its value after the array is cleared.

In a rectangular growable local, each outer cell holds the descriptor
of one row. Each row owns its child buffer. Inner extents in the outer
descriptor distinguish an established length of zero from an unknown
length. Clearing the outer array releases its rows and retains those
extents for later insertions. Copying the array walks the row descriptors
and allocates independent buffers down to the leaf cells. See the
`row runtime <../../../Runtime/src/array_rows.c>`_.

The byte stride is the checked allocation size of one element. A boolean
uses one byte, and a character uses four bytes for its Unicode scalar
value. The strides of 16-, 32-, and 64-bit floats are two, four, and
eight bytes respectively. Each buffer uses the element alignment for
the selected target.

Nullable cells store the presence byte, alignment padding, and payload
together. The element stride includes all three. Enum cells hold the
case tag, and integer range cells hold both endpoints and the end
inclusion flag. The `nullable cell tests
<../../../Tests/Compiler/Driver/Inputs/growable-nullable-cell-values.co>`_
exercise these layouts across insertion, removal, and copying.

A class or interface reference occupies eight bytes on both pointer
widths. Array assignment copies that handle into each destination cell.
The low 32 bits identify a registry slot and the high 32 bits identify
its generation. Deletion advances the generation before the slot can be
reused. The `object runtime <../../../Runtime/src/object.c>`_ retires a
slot when its generation reaches the limit. Zero denotes an absent
handle at the runtime boundary.

Before allocating, the runtime checks that capacity times stride, plus
the padding needed to align the data pointer, fits ``PTRDIFF_MAX``.
A grow operation
preserves the existing cells. Insertion shifts the suffix right if
capacity is available. When growing the buffer, it copies the prefix,
inserted cell, and suffix into their final positions. Removal copies the
selected cell into the caller's output slot before shifting the suffix
left. Growable locals retain their allocation and capacity across removals
and ``clear()``, including when their length reaches zero. Releasing the
descriptor frees its owner and clears all four words. See the `array runtime
<../../../Runtime/src/array.c>`_ and `integer storage layout <Integers.rst>`_.

The `storage layout implementation <../../../Compiler/src/semantic/storage_layout.cpp>`_
calculates field offsets and array storage sizes. The `layout tests
<../../../Tests/Compiler/Semantic/Unit/storage_layout_test.cpp>`_ include an
array field on both target widths.
