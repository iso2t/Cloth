.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Integer storage layout
----------------------

Signed and unsigned integers can be 1 through 65,536 bits wide. Integers
of the same width have the same layout.
Round the bit width up to whole bytes, then round the byte count up to a
multiple of the target integer alignment. This allocation size is both
the scalar's ``::size`` and the stride between consecutive array elements.
The array's own ``::size`` counts elements.

On the Windows and Linux x86 targets, widths up to 8, 16, and 32 bits
align to 1, 2, and 4 bytes respectively. Widths from 33 through 64 align
to 8 bytes, except on 32-bit x86 System V where they align to 4. Wider
integers align to 16 bytes. A fixed-size array stores its elements
consecutively. Three ``int<12>`` elements occupy 6 bytes, starting at
offsets 0, 2, and 4. For ``int<65536>`` the first two elements start at
byte offsets 0 and 8,192.

For the offsets of integer fields within a struct, see `Aggregate
storage layout <Aggregates.rst#struct-fields>`_.

The compiler calculates integer alignment, array stride, and struct field
positions in the `storage layout implementation
<../../../Compiler/src/semantic/storage_layout.cpp>`_.
