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
of these types by value. Class references and nullable class references
also pass by value. A helper can return one such value or ``void``.

Local values
------------

The scalar types accepted for helper arguments can also be stored in
locals, including nullable values. Native string locals accept literals,
copies, and conditional results. In the `string size example
<../../../Tests/Compiler/Driver/Inputs/string-size.co>`_, ``text::size``
returns 0 for ``""`` and 5 for ``"Cloth"``. See `data providers
<../../language/expressions/Access.rst#data-providers>`_ for how Unicode is counted.

A struct local can contain scalars, strings, integer ranges, and nested
structs, including nullable forms of these fields. It can also contain
fixed-size arrays, including whole nullable array fields. Native enum
payloads can use scalar and string literals, fixed-size array literals,
and structs built from them.
A nullable enum field can default to ``null``.
For field assignment and copying, see `Structs
<../../language/types/Declarations.rst#structs>`_. For byte sizes and
field offsets, see `Aggregate storage layout <../storage/Aggregates.rst>`_.

Native code supports guarded field reads on nullable structs, including
chains such as ``box?.sample?.value``. See `guarded field reads
<../../language/expressions/Access.rst#guarded-field-reads>`_.

Class references
----------------

Fixed-size and growable arrays can hold class references. Copying an
array preserves each object's identity. The `class array example
<../../../Tests/Compiler/Driver/Inputs/class-array-copy.co>`_ constructs
two instances at one call site and uses ``===`` to check which one each
array holds after replacement. Native construction supports a fieldless,
non-generic class with a parameterless primary constructor and no explicit
parents, state, or destructor. A public secondary constructor can take
explicit arguments and delegate to that primary with ``: () {}``.

Fixed-size arrays
-----------------

Native code supports indexed reads and writes on rectangular fixed-size
integer array locals. For example, ``int[2][3]`` holds two rows of three
integers, and ``rows[1][2] = 7`` updates the last cell. See `rank and
extents <../../language/types/Arrays.rst#rank-and-extents>`_ for dimension
order and `indexing and updates
<../../language/types/Arrays.rst#indexing-and-updates>`_ for evaluation order.

Copying a struct with a fixed-size array field copies its cells. In the
`field copy example <../../../Tests/Compiler/Driver/Inputs/fixed-array-fields.co>`_,
changing a cell in the original leaves the copy intact. A field declared
as ``int[2]? cells`` can also be cleared with ``sample.cells = null``.
An earlier copy keeps both its presence and its cells. The
`enum payload example <../../../Tests/Compiler/Driver/Inputs/enum-aggregate-copy.co>`_
shows this with a struct read from an enum case.

Native fixed-size arrays can themselves hold structs with fixed-size
array fields, including nullable fields. You can replace an indexed
struct, change a cell through ``rows[i].cells[j]``, or iterate the
structs by value. The `array of structs example
<../../../Tests/Compiler/Driver/Inputs/array-of-structs.co>`_ replaces a
row and returns 42 from the saved array.

Fixed-size arrays support ``==`` and ``!=`` across one or more dimensions.
For example, a two-by-three integer array compares unequal to a three-by-two
array holding the same six values. Supported cell types are integers,
booleans, characters, floats, integer ranges, and enums with built-in
equality. Cells can be nullable, as in ``int?[2][3]`` and
``range<int>?[3]``. See `array equality
<../../language/expressions/Operators.rst#array-equality>`_ for comparison
rules and `arrays <../../language/types/Arrays.rst>`_ for copying, indexing,
and updates.

Native locals can hold a whole nullable fixed-size array, as in
``int[2]? pair = null``. Use ``== null`` or ``!= null`` to test presence,
or select an array with ``pair ?: [0, 0]``. Native ``==`` and ``!=`` also
compare two array values when either or both are nullable. Two absent
arrays compare equal. Two present arrays compare by shape and cells.
See `nullable arrays
<../../language/types/Arrays.rst#nullable-arrays>`_.

Native arrays also support nullable fixed-size rows. You can copy,
replace, or clear individual rows and iterate them with a nullable row
binding. The `nullable row example
<../../../Tests/Compiler/Driver/Inputs/nullable-array-rows.co>`_ returns
42 from a saved row after clearing it in the original array.

.. _growable-integer-arrays:
.. _growable-scalar-arrays:

Growable array locals
---------------------

Native code supports one-dimensional growable locals with ``bool``,
``char``, floating-point, integer, enum, integer range, or class-reference
elements, including nullable forms. The `nullable cell example
<../../../Tests/Compiler/Driver/Inputs/growable-nullable-cells.co>`_
inserts 42 between ``null`` and zero, then removes and returns it.
Floats may be 16, 32, or 64 bits wide. Signed and unsigned integers may
have any supported width, including the target-sized ``isize`` and ``usize``.

Use ``append``, ``insert``, ``removeAt``, ``clear``, and ``reserve`` on a
mutable local. Read the length with ``::size`` and read or update
individual cells by index. Assignment copies the elements. A growable
array and a fixed-size array with the same element type can compare
equal. See `growable arrays <../../language/types/Arrays.rst#growable-arrays>`_
and `array equality <../../language/expressions/Operators.rst#array-equality>`_.

Link ``Runtime/src/array.c`` and ``Runtime/src/errors.c`` with the emitted
object. The `growable array example
<../../../Tests/Compiler/Driver/Inputs/growable-array.co>`_ appends a second
element and returns it as the process status, 42. The `insertion and
removal example <../../../Tests/Compiler/Driver/Inputs/growable-array-mutations.co>`_
returns 42 after clearing and reusing the same array.
The `scalar array example
<../../../Tests/Compiler/Driver/Inputs/growable-scalar-arrays.co>`_
removes a NUL character, compares both signs of floating-point zero, and
returns the removed floating-point value as exit status 42.

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

Native ``for`` loops accept integer ranges, fixed-size arrays with
supported element types, and one-dimensional `growable array locals`_.
A rectangular array yields a writable copy of each row. The `growable
array example <../../../Tests/Compiler/Driver/Inputs/growable-array-traversal.co>`_
returns 42 while clearing the source inside the loop. See `array iteration
<../../language/statements/ControlFlow.rst#array-iteration>`_.

Write ``for (int n <- 13..16)`` to iterate a range directly, or bind
``def numbers := 13..16`` and use
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
``println(int)``, ``exit(status)``, ``exit(status, message)``, and ``toInt()`` on a
character or boolean. See the `object emission diagnostics
<../../../Tests/Compiler/Driver/emit-object-errors.test>`_.

Programs using stored class fields, constructor bodies with statements,
explicit destruction, method dispatch, state transitions,
calls to generic functions, multidimensional growable arrays and the collection APIs can
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
* Both forms of ``exit`` use `Runtime/src/process.c <../../../Runtime/src/process.c>`_.
  An optional string message is written to standard output before the
  process exits. See `Ending the process
  <../../language/expressions/Builtins.rst#ending-the-process>`_ for the byte
  and newline behavior.
* Integer division and remainder checks, negative integer exponent checks,
  float-to-integer conversion checks, array bounds checks, and range
  steps that can stall use `Runtime/src/errors.c <../../../Runtime/src/errors.c>`_.
* Growable arrays use `Runtime/src/array.c <../../../Runtime/src/array.c>`_
  for allocation, copying, and release, and `Runtime/src/errors.c
  <../../../Runtime/src/errors.c>`_ for capacity, allocation, bounds, and length errors.
* Creating a native class identity uses `Runtime/src/object.c
  <../../../Runtime/src/object.c>`_ and `Runtime/src/errors.c
  <../../../Runtime/src/errors.c>`_.
* When a `range counter <../../language/statements/ControlFlow.rst#integer-ranges>`_
  is wider than the target pointer, ``*=``, ``/=``, and ``%=`` steps also
  need `Runtime/src/integer.c <../../../Runtime/src/integer.c>`_.
* On Unix, floating-point remainder and exponentiation can require the
  platform math library. Put ``-lm`` after the object file when linking.
