.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Emitting object files and assembly
==================================

Compile the source file that defines ``main``. The emitted object or
assembly contains the entry point and the functions it calls. Link an
object with a C compiler for the same platform and width to produce an
executable. For a complete example, see `Run your first Cloth program
<../../getting-started/FirstProgram.rst>`_.

Entry point and output
----------------------

For native code, ``main`` takes no parameters and returns a signed
32-bit ``int``. Its body can be an expression or a block with
`conditionals and loops <../../language/statements/ControlFlow.rst>`_.
When checking an executable without emitting native code, Cloth accepts
additional `entry-point signatures <../Flags.rst#program-kind>`_.

Use ``--emit object`` to write an object beside the source. To inspect
assembly, use ``--emit assembly``. These commands write a listing to an
explicit path and assemble it for the native target with LLVM's assembler.

.. code-block:: sh

   ./build/cloth --kind executable --src build/main.co --emit assembly -o build/listing.s
   llvm-mc --filetype=obj build/listing.s -o build/listing.o

The destination directory must already exist. A successful write
replaces an existing regular file. If compilation fails, the old file
keeps its contents. Cloth rejects paths that would overwrite the entry
source or any of its imports.

On x86, ``--target-bits 32`` selects i386 and ``--target-bits 64``
selects x86-64. The toolchain used to build Cloth determines the object
format. Linux uses ELF and Windows uses COFF. The default width is the
native pointer width. The choice also affects `integer size and alignment
<../storage/Integers.rst>`_.

Functions in another file
-------------------------

Save these two files in the same directory. In ``build/helpers.co``,
declare the function ``pub`` so another Cloth file can import it.

.. code-block:: text

   pub fn<int> identity(int value) = value

In ``build/main.co``, import the function by name.

.. code-block:: text

   using helpers: identity

   pub fn<int> main () = identity(42)

From the repository root, compile the entry file and link the resulting
object.

.. code-block:: sh

   ./build/cloth --kind executable --src build/main.co --emit object
   cc build/main.o -o build/main

With MinGW, use ``gcc build/main.o -o build/main.exe`` for the link
command. The program exits with status 42. Both functions are in
``main.o``. Only ``main`` has external linkage. The imported ``identity``
is local to the object. See the `two source files
<../../../Tests/Compiler/Driver/Inputs/imported-calls/>`_.

Native code can call non-generic, top-level functions in the same file
or in an imported file. Supply every argument explicitly. Helpers take
signed and unsigned integers of any Cloth width, ``isize``, ``usize``,
``bool``, ``char``, 16-, 32- or 64-bit floats, enums, and nullable forms
of these types by value. A helper can return one such value or ``void``.

Local values
------------

The scalar types accepted for helper arguments can also be stored in
locals, including nullable values. A struct local can contain these
types, integer ranges, and other supported structs. Each field can be
nullable. Native enum payload reads support integer, boolean, character,
and floating-point constants, their nullable forms, and structs built
from those constants. A nullable enum field can default to ``null``.
For field assignment and copying, see `Structs
<../../language/types/Declarations.rst#structs>`_. For byte sizes and
field offsets, see `Aggregate storage layout <../storage/Aggregates.rst>`_.

Fixed-size arrays
-----------------

Native code supports rectangular fixed-size integer array locals and
indexed reads. For example, ``int[2][3]`` holds two rows and each row has
three valid column indices. See `rank and extents
<../../language/types/Arrays.rst#rank-and-extents>`_.

One-dimensional fixed-size arrays support ``==`` and ``!=`` for integers,
booleans, characters, floats, integer ranges, and enums that use built-in
equality. The elements can be nullable, as in ``int?[2]`` and
``range<int>?[3]``. See `array equality
<../../language/expressions/Operators.rst#array-equality>`_ for comparison
rules and `arrays <../../language/types/Arrays.rst>`_ for copying, indexing,
and updates.

.. _integer-range-membership:

Integer range comparisons
-------------------------

Native code supports ``value : range`` and compares integer ranges with
``==`` and ``!=``, including when either or both operands are nullable.
Ranges can be written inline or stored in locals and struct fields.
Their endpoints can be signed or unsigned integers of any Cloth width,
including ``isize`` and ``usize``. See `Ranges and membership
<../../language/expressions/Operators.rst#ranges-and-membership>`_.

Test a nullable range with ``bounds == null`` or ``bounds != null``, or
use ``?:`` to select a fallback range. The `nullable range example
<../../../Tests/Compiler/Driver/Inputs/nullable-integer-range.co>`_ returns 42.

Loop sources
------------

Native ``for`` loops accept integer ranges and one-dimensional fixed-size
arrays with supported element types. Write ``for (int n <- 13..16)`` to
iterate a range directly, or bind ``def numbers := 13..16`` and use
``for (int n <- numbers)`` for each traversal. The `stored range example
<../../../Tests/Compiler/Driver/Inputs/stored-integer-range-loop.co>`_
compiles to an object and returns 42 when linked and run.

Source checks and native code
-----------------------------

Checking a file examines function bodies throughout its imports. Emitting
an object starts at ``main`` and follows calls to the helpers used by that
program. A library declaration can therefore pass source checking even
when a native caller cannot use it.

For example, ``println(true)`` and ``int[2]::type::rank`` both pass source
checking. Either expression in a reachable body produces ``C4040`` during
object emission. ``println(true.toInt())`` has a native implementation.
Of the built-in functions and primitive methods, native code supports
``println(int)``, ``exit(status)`` with one integer, and ``toInt()`` on a
character or boolean. See the `object emission diagnostics
<../../../Tests/Compiler/Driver/emit-object-errors.test>`_.

Programs using class construction and method dispatch, state transitions,
calls to generic functions, growable arrays and the collection APIs can
be checked with ``--src``. The same applies to the standard library's file
and UTF-8 intrinsics. The native call and storage forms are described
above. See the `standard library guide <../../../Stdlib/README.md>`_ for
the module interfaces.

Runtime dependencies
--------------------

Include the runtime sources for the operations your program uses in the
C compiler's link command. For the effects of printing and exiting, see
`Built-in functions and methods <../../language/expressions/Builtins.rst>`_.

* ``println`` takes a signed 32-bit ``int``. On both target widths its
  C entry point is ``void cloth_rt_println_i32(int32_t value)`` in
  `Runtime/src/println.c <../../../Runtime/src/println.c>`_.
* ``exit(status)`` uses `Runtime/src/process.c <../../../Runtime/src/process.c>`_.
* Integer division and remainder checks, negative integer exponent checks,
  float-to-integer conversion checks, array bounds checks, and range
  steps that can stall use `Runtime/src/errors.c <../../../Runtime/src/errors.c>`_.
* When a `range counter <../../language/statements/ControlFlow.rst#integer-ranges>`_
  is wider than the target pointer, ``*=``, ``/=``, and ``%=`` steps also
  need `Runtime/src/integer.c <../../../Runtime/src/integer.c>`_.
* On Unix, floating-point remainder and exponentiation can require the
  platform math library. Put ``-lm`` after the object file when linking.
