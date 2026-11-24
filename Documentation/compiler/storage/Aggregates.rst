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

An integer range field stores the start, end, and one-byte end-inclusion
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

The element buffer occupies the element count times the allocation size
of one element. For integer elements, see `Integer storage layout <Integers.rst>`_.

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

The `storage layout implementation <../../../Compiler/src/semantic/storage_layout.cpp>`_
calculates field offsets and array storage sizes. The `layout tests
<../../../Tests/Compiler/Semantic/Unit/storage_layout_test.cpp>`_ include an
array field on both target widths.
