.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

==============  =================================================================  ============
Flag            Behavior                                                           Return code
==============  =================================================================  ============
``-l``          Selects lexer-only mode and exits silently.                        ``0xD00001``
``-L``          Prints the lexer token table to standard output.                   ``0xD00002``
``--src PATH``  Reads a UTF-8 ``.co`` or ``.clib`` file. Quote paths with spaces.  By mode
==============  =================================================================  ============

``--target-bits 32`` and ``--target-bits 64`` select the width used to check
target-sized integers and array extents. The default is the native pointer width.

``--stdlib PATH`` adds a standard library source root. Imports search the entry
directory first. Collection iteration uses the Snapshot declaration supplied
by the configured standard library.

``--kind library`` checks declarations and bodies. This is the default, and
helper-only sources are valid. ``--kind executable`` also requires exactly one
top-level ``main`` in the entry file. It can take no parameters or one
``string[]`` or ``string[]?`` parameter and can return ``void``, ``isize``, or
a signed ``int`` at a supported width. Both modes run the frontend checks.

An intrinsic body must come from its registered module imported through
``--stdlib``. The frontend checks the declaration and its full signature
before recording the operation for later compilation stages.

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
