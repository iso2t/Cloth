.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Integer storage layout
----------------------

Signed and unsigned integers of the same width have the same layout.
Round the bit width up to whole bytes, then round the byte count up to a
multiple of the target integer alignment. This allocation size is both
the scalar's ``::size`` and the stride between consecutive array elements.
The array's own ``::size`` counts elements.

On the Windows and Linux x86 targets, widths up to 8, 16, and 32 bits
align to 1, 2, and 4 bytes respectively. Widths from 33 through 64 align
to 8 bytes, except on 32-bit x86 System V where they align to 4. Wider
integers align to 16 bytes. For example, ``int<12>::size`` is 2 and
three elements occupy 6 bytes in an array's data buffer.

Struct fields appear in declaration order. Each starts at its required
alignment, and the struct's total size is rounded up to its largest field
alignment. Fields of types ``int<8>``, ``int<12>``, and ``int<8>`` therefore
start at byte offsets 0, 2, and 4. The struct occupies 6 bytes including padding.

The compiler calculates integer alignment, array stride, and struct field
positions in the `storage layout implementation
<../../../Compiler/src/semantic/storage_layout.cpp>`_.
