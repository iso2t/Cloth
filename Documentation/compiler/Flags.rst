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
