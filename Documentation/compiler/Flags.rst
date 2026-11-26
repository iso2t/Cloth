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
the entry file. The entry and all imported modules use the same grammar.
Terminate each import and simple statement with ``;``. See `Source files
and literals <../language/source/Source.rst#names-and-statement-boundaries>`_
for statement boundaries and `Names and visibility
<../language/source/Visibility.rst>`_ for import selection.

Imports search the entry directory first, then the standard library.
By default, Cloth finds the library in ``lib/cloth/stdlib`` relative to
the compiler executable. Use ``--stdlib PATH`` to select a different
directory containing ``cloth/``, such as ``Stdlib/StandardLibrary/src``
when developing the library. The selected library also supplies the
declaration for `collection iteration
<../language/statements/ControlFlow.rst#collection-iteration>`_.

Migrating legacy semicolons
---------------------------

Build the standalone converter with ``make migrate-semicolons`` or
``mingw32-make migrate-semicolons`` on Windows. Run it on individual
``.co`` or ``.clib`` files, or on directories to process them recursively.
For a checkout known to use the legacy grammar, use the following command.

.. code-block:: console

   build/cloth-migrate-semicolons --from-legacy src

The converter reads the legacy parse first and verifies that each proposed
file has the same syntax tree under strict parsing. It replaces each verified
file atomically. A second run leaves its bytes and modification time intact.
Without ``--from-legacy``, a file that parses both ways with different meanings
produces a diagnostic at the conflicting source range. A parse failure in both
grammars or a rewrite that changes the tree also leaves that file untouched.
The command exits with status 1 if any input needs attention.

For example, the legacy initializer ``Box x := {;}`` creates an empty
deferred binding. Removing its interior semicolon would make ``{}`` a
struct literal in strict syntax. The converter reports the difference
and preserves the original file.

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

.. _object-emission:

Native output
-------------

``--emit object`` writes a relocatable object file. ``--emit assembly``
writes assembly for the selected target. Both require ``--src PATH`` and
either ``--kind executable`` or a library ``--module-name NAME``.
Lexer modes stop before code generation. Use ``--extern NAME=SOURCE`` to
bind a separately built library and ``--print-target`` to print the target
triple. See `Native libraries <driver/Objects.rst#native-libraries>`_.

By default, Cloth replaces the source file's extension with ``.o`` or
``.s`` in the same directory. For ``src/main.co``, assembly goes to
``src/main.s``. Use ``-o PATH`` or ``--output PATH`` to choose the full
output path, including its extension.

An output path alone selects the default object format. The legacy
spelling ``--emit-object PATH`` is an alias for ``-o PATH``. With no
emission or output option, the driver checks the source. For compile
and link commands, see `Emitting object files and assembly <driver/Objects.rst>`_.

.. _native-values-and-operations:

For the behavior of values in source, see the `language reference
<../language/index.rst>`_. For byte sizes and field offsets, see
`Aggregate storage layout <storage/Aggregates.rst>`_.
