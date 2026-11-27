.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Reading compiler diagnostics
============================

Run ``cloth --src file.co`` to check a source file. On a source error,
Cloth writes to standard error and exits with status 1. The report gives
a stable Cloth code, a message and, when source is available, the location
that triggered it. A second label can point into another file, such as an
imported declaration. The numeric part of a code identifies its entry
in the `diagnostic code registry <../../../Diagnostics/include/cde/diagnostic_code.hpp>`_.
Errors opening an input file can also appear as plain text.

A missing semicolon
-------------------

Given ``fn<int> answer() = 42 // result``, the compiler reports ``C2013``
and points immediately after ``42``. Write ``42; // result`` to terminate
the function before the comment. The same diagnostic applies when the
file is reached through an import.

An argument supplied twice
--------------------------

The following call supplies ``start`` both by position and by name.

.. code-block:: text

   fn take(opt<int> start ?: 0, int count) {}
   fn run() = take(1, 2, start = 3);

To see the report from the repository root, run this command with your
built ``cloth`` executable.

.. code-block:: sh

   cloth --src Tests/Compiler/Semantic/argument-binding.co

The first line reads ``[C3065] Error: Parameter 'start' is supplied twice``.
The fixture includes a license header, so ``argument-binding.co:4:23``
points to the named argument on line 4. The label ``declared here``
points back to the parameter in ``take``. Give each parameter one value.
For example, this version passes 3 for ``start`` and 2 for ``count``.

.. code-block:: text

   fn take(opt<int> start ?: 0, int count) {}
   fn run() = take(count = 2, start = 3);

With a repeated import such as ``using shapes: Point, Point;``, ``C2010``
marks the second ``Point`` and labels the first selection. See the
`import diagnostic <../../../Tests/Diagnostics/repeated-import.test>`_.
Use `lexer mode <../Flags.rst#lexer-modes>`_ to inspect the tokens when a
literal or a statement boundary is in doubt. A lexer-only run stops
before parsing the file.

Object emission and runtime errors
----------------------------------

A program can pass source checking and report an error when compiled
with ``--emit-object``. For example, the frontend accepts ``1 -> Number``.
Object emission reports ``C4040`` at the cast because materializing an
integer as an interface value requires native boxing. See
`Emitting and linking object files <Objects.rst>`_.

A runtime error comes from the linked program. In the `division example
<../../../Tests/Compiler/Driver/zero-divisor.test>`_, the program prints
``84`` and ``2``, then writes ``Cloth runtime error: integer division by zero``
to standard error and exits with status 1. If the C linker reports an
undefined ``cloth_rt_`` symbol before an executable exists, match the
operation to its `runtime source <Objects.rst#runtime-dependencies>`_.
