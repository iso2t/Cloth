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

A native ``main`` can take no parameters or receive the command-line
arguments as ``string[]`` or ``string[]?``. The first element is the
first argument after the executable name. With no arguments, a
``string[]`` parameter receives an empty array and a ``string[]?``
parameter receives ``null``. Passing one empty string produces a
present array of length one. For example, this program returns the
number of arguments.

.. code-block:: text

   pub fn<isize> main(string[] args) = args::size -> isize;

Arguments are UTF-8 strings. On Windows the runtime reads the wide
command line. On Linux it validates the supplied bytes. Invalid input
produces a runtime error before Cloth enters ``main``. Link a program
that takes arguments with ``Runtime/src/arguments.c``,
``Runtime/src/array.c``, and ``Runtime/src/errors.c``. On MinGW add
``-lshell32``. See the `linked argument examples
<../../../Tests/Compiler/Driver/entry-arguments.test>`_.

A ``void`` entry returns status zero. An ``isize`` or signed ``int``
result becomes a signed 32-bit native status. Narrow values are sign
extended and wider values contribute their low 32 bits. Windows reports
all 32 bits. A Linux parent observes the low eight bits.

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

Native libraries
----------------

Give a library entry file a module name to emit its public, concrete
top-level functions. The same name binds the library when compiling a
consumer. The examples below use the sources from the
`native library fixture <../../../Tests/Compiler/Driver/Inputs/native-library>`_.

.. code-block:: sh

   ./build/cloth --kind library --module-name Arithmetic --src Tests/Compiler/Driver/Inputs/native-library/api.co -o build/arithmetic.o
   ./build/cloth --kind executable --extern Arithmetic=Tests/Compiler/Driver/Inputs/native-library/api.co --src Tests/Compiler/Driver/Inputs/native-library/main.co -o build/answer.o
   cc build/answer.o build/arithmetic.o -o build/answer

The consumer selects both ``pick(int)`` and ``pick(bool)`` and returns 42.
Imports from a library resolve relative to its entry directory and the
configured Stdlib root. Callers can use defaults from the library.
The source-backed interface also supplies generic bodies for local
specialization.

The native export ABI supports integers, floats, booleans, characters
and strings, their nullable forms, and multiple results of those types.
Functions may also return ``void``. An unsupported concrete export is
diagnosed at its declaration. Ordinary source imports still support
the full set of types the backend can lower within one object.

For calls that use the runtime, pass the target's
``libcloth_runtime.a`` after the objects on the linker command line.
The `Shuttle build command <../../../Shuttle/README.rst>`_ selects the
archive and system libraries automatically.

Functions in another file
-------------------------

Save these two files in the same directory. In ``build/helpers.co``,
declare the function ``pub`` so another Cloth file can import it.

.. code-block:: text

   pub fn<int> identity(int value) = value;

In ``build/main.co``, import the function by name.

.. code-block:: text

   using helpers: identity;

   pub fn<int> main () = identity(42);

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

Native code can call top-level functions in the same file
or in an imported file. Positional and named arguments run in written
order. Omitted defaults run next, in parameter order, and can read
earlier parameters. Helpers accept and return the native local value
types described below. Array parameters and results have independent
cells. Copying a class reference preserves the object's identity.

Calls to generic functions and directly selected methods use the concrete
types inferred or written at the call. A numeric bound can use the
standard library's ``Number``. Arithmetic uses the
concrete integer width and signedness, floating-point operation, or selected
operator body. See `generic types and functions <../../language/types/Generics.rst>`_.

For a virtual or interface call, the receiver's concrete class supplies
the implementation. The type at the call site supplies the argument
names and defaults. A class can inherit an interface's default body.
The `method dispatch example <../../../Tests/Compiler/Driver/Inputs/method-dispatch.co>`_
calls the same two objects through both a base class and an interface.

A state method uses the state active when the call takes place.
An alias observes a transition on the same instance. If the active state
omits the requested method, the runtime reports
``Cloth runtime error: method is unavailable in the active state``.
See `named states <../../language/objects/States.rst>`_.

A helper can return several values. For example, ``int low, int high =
bounds()`` receives both results from one call. Use ``_, high = bounds()``
to retain only the second. A deferred block runs again each time a bound
name is read. See `calls and results <../../language/expressions/Calls.rst>`_
and the `linked evaluation-order example
<../../../Tests/Compiler/Driver/Inputs/call-results.co>`_.

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
array holds after replacement.

Native code can construct classes with stored fields, an
inherited class, and public secondary constructors. Class and interface
references share the same eight-byte handle on both target widths.
You can call directly selected class methods, test a reference with
``is``, and delete through a class or interface alias. The `linked
construction and deletion example <../../../Tests/Compiler/Driver/Inputs/class-lifetime.co>`_
shows inherited field access and destructor order. For calls that can
invalidate their own receiver, see `Object ownership
<../../language/objects/Ownership.rst>`_.

Generic class and struct fields use their concrete element types. Calling
a struct method on a mutable local, a stored field, or a fixed-array cell
can change that value. An earlier copy keeps its own fields. On a nullable
struct, a guarded call changes the stored payload only when it is present.
See the `generic struct and class example <../../../Tests/Compiler/Driver/Inputs/generic-specialization.co>`_.

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

The native collection example updates ``list[0]``,
visits its snapshot while clearing the list, and exits with status 42.
See the `collection consumer <../../../Tests/Compiler/Driver/Inputs/stdlib-collections.co>`_
and the `standard library guide <../../../Stdlib/README.md>`_.

The frontend also accepts primitive boxing into an interface value,
multidimensional growable arrays, and floating-point range membership.
A reachable use of these forms can produce an object emission diagnostic.
See the `driver tests <../../../Tests/Compiler/Driver/emit-object-errors.test>`_.

Runtime dependencies
--------------------

Include the runtime sources for the operations your program uses in the
C compiler's link command. For the effects of printing and exiting, see
`Console functions, exit, and primitive methods <../../language/expressions/Builtins.rst>`_.

* The standard-library console functions, formatting, string comparison and methods, Unicode
  character methods, and UTF-8 decoding use `Runtime/src/text.c
  <../../../Runtime/src/text.c>`_, the array and error runtimes, and the
  bundled `utf8proc source <../../../ThirdParty/utf8proc/utf8proc.c>`_.
  Compile these sources with ``-IRuntime/include``, ``-IThirdParty/utf8proc``
  and ``-DUTF8PROC_STATIC``. The numeric and metadata runtime checks also use
  the error reporter.
* File access uses `Runtime/src/files.c <../../../Runtime/src/files.c>`_
  with the object, array, and error runtimes. A file closes when its Cloth
  object is deleted or the process releases its live objects.
* Both forms of ``exit`` use `Runtime/src/process.c <../../../Runtime/src/process.c>`_.
  An optional string message is written to standard output before the
  process exits. See `Ending the process
  <../../language/expressions/Builtins.rst#ending-the-process>`_ for the byte
  and newline behavior.
* Integer division and remainder checks, negative integer exponent checks,
  float-to-integer conversion checks, array bounds checks, range steps
  that can stall, and ambiguous reference equality use
  `Runtime/src/errors.c <../../../Runtime/src/errors.c>`_.
* Growable arrays use `Runtime/src/array.c <../../../Runtime/src/array.c>`_
  for allocation, copying, and release, and `Runtime/src/errors.c
  <../../../Runtime/src/errors.c>`_ for capacity, allocation, bounds, and length errors.
* Class allocation, access, deletion, method and state dispatch, and struct
  methods that check their receiver's owning object use `Runtime/src/object.c
  <../../../Runtime/src/object.c>`_. Pass ``-IRuntime/include`` when
  compiling the runtime from the repository root. Classes with growable
  array fields or growable state payloads also use the array runtime.
* When a `range counter <../../language/statements/ControlFlow.rst#integer-ranges>`_
  is wider than the target pointer, ``*=``, ``/=``, and ``%=`` steps also
  need `Runtime/src/integer.c <../../../Runtime/src/integer.c>`_.
* Counting leading or trailing zero bits on an integer wider than 64 bits
  also uses `Runtime/src/integer.c <../../../Runtime/src/integer.c>`_.
* On Unix, floating-point remainder and exponentiation can require the
  platform math library. Put ``-lm`` after the object file when linking.
