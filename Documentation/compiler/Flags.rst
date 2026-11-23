.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Compiler flags
==============

Use ``cloth --src PATH`` to check a source file. A successful source
check is silent and returns 0. A compilation error is reported on
standard error and returns 1.

Source files and imports
------------------------

``--src PATH`` selects a UTF-8 ``.co`` or ``.clib`` entry file. Quote
paths that contain spaces. The compiler follows ``using`` imports from
the entry file. See `Names and visibility <../language/source/Visibility.rst>`_.

``--stdlib PATH`` sets the standard library source root. Imports search
the entry directory first, then this root. To use the library in this
checkout, pass ``--stdlib Stdlib`` from the repository root. This
supplies the standard library declaration for `collection iteration
<../language/statements/ControlFlow.rst#collection-iteration>`_.

Lexer modes
-----------

Both lexer modes stop after scanning the source.

.. list-table::
   :header-rows: 1

   * - Option
     - Output on success
     - Driver return code
   * - ``-l``
     - No output
     - ``0xD00001``
   * - ``-L``
     - Token table on standard output
     - ``0xD00002``

Unix shells expose the low eight bits of these codes, 1 and 2
respectively.

Program kind
------------

``--kind library`` checks declarations and bodies. This is the default.
A file containing only helper functions is a valid library.

``--kind executable`` also requires exactly one top-level ``main`` in the
entry file. It can take no parameters or one ``string[]`` or ``string[]?``
parameter. Its return type can be ``void``, ``isize``, or a signed ``int``
at a supported width. For the signature used in native object files,
see `Entry point and output <driver/Objects.rst#entry-point-and-output>`_.

Target width
------------

``--target-bits 32`` and ``--target-bits 64`` select the width for
``isize``, ``usize``, and `array extents
<../language/types/Arrays.rst#rank-and-extents>`_. The default is the
native pointer width.

Object emission
---------------

``--emit-object PATH`` writes a relocatable object at the requested
path. It requires ``--src`` and ``--kind executable`` and is incompatible
with ``-l`` and ``-L``. For target formats and linking, see
`Emitting and linking object files <driver/Objects.rst>`_.

.. _native-values-and-operations:

For the behavior of values in source, see the `language reference
<../language/index.rst>`_. For byte sizes and field offsets, see
`Aggregate storage layout <storage/Aggregates.rst>`_.
