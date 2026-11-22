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

Object emission
---------------

With ``--kind executable``, add ``--emit-object PATH`` to write a relocatable
object after the frontend checks succeed. The emitter supports a parameterless
``main`` returning a signed 32-bit ``int``. Its body can be an expression
or a block. An ``if``/``else`` can produce a scalar value, as in
``return if (flag) 42 else 7``. Only the selected arm executes. Its last
expression supplies the value unless the arm returns from the function.
A ``while`` loop tests its condition before each iteration. If the first test
is false, execution continues after the loop without entering its body.
A ``do``/``while`` loop runs its body before the first test and repeats while
the condition is true. A return from the body skips the condition.
``break`` exits the loop. ``continue`` skips the rest of its body and jumps
to its condition. A jump inside a nested loop's condition targets the
enclosing loop body. A guard such as ``if (done) break`` may omit ``else``.
Scalar operators evaluate their operands in source order. If an operand
executes ``return``, ``break`` or ``continue``, earlier side effects are preserved.
Execution skips the remaining operands and the enclosing operation.

Locals and non-generic helper functions can use signed and unsigned integers
of any Cloth width, ``isize``, ``usize``, ``bool``, ``char`` and 16-, 32- or
64-bit floats. A mutable local supports assignment, compound arithmetic
assignment, increment and decrement.
A local declared with ``:=`` keeps its initial value. A ``def`` binding infers
its type from the initializer.

Integer arithmetic, including ``**``, wraps at the operand width. On both
targets ``int<12>`` wraps from 2047 to -2048. The width of ``isize`` and
``usize`` follows ``--target-bits``. Unary ``~`` flips every bit at that width.
Signed division truncates toward zero, and a nonzero remainder has the sign
of the dividend. If an integer divisor evaluates to zero, ``/``, ``%``,
``/=`` and ``%=`` write ``Cloth runtime error: integer division by zero`` to
standard error and exit with status 1. Both operands run once in source
order before the check.
Signed division and remainder trap on the signed minimum divided by minus one.
Integer exponentiation traps on a negative exponent. The frontend diagnoses
literal zero divisors and literal signed division overflow with ``C3050`` and ``C3051``.

Comparisons use the checked operand type. Integer-to-boolean conversion
maps zero to false and every nonzero value to true, including at parameter
and return boundaries. Booleans occupy one byte. Built-in ``&&`` and ``||``
evaluate their right operand only when needed. A character holds one Unicode
scalar value. The ``toInt()`` method returns its code point or a boolean's
normalized integer value.

Numeric casts use the destination width. Floating-to-integer casts truncate
toward zero. If the operand is NaN or infinite, or the truncated integer is
outside the destination range, the cast writes
``Cloth runtime error: invalid float-to-integer cast (NaN or out of range)``
to standard error and exits with status 1. For example, ``-128.75``
converts to ``-128`` in ``int<8>``, while ``-129.0`` reports the error.

Both the integer division and float-to-integer runtime errors flush earlier
standard output before writing the diagnostic. Link objects that use either
check with ``Runtime/src/errors.c``, for example ``cc main.o Runtime/src/errors.c -o main``.

Floating comparison follows IEEE rules, including unordered inequality for
NaN. Floating remainder and exponentiation can require the platform math
library. On Unix, link these objects with ``-lm``.

A call can reach a function in the same file or a ``pub`` function imported
with ``using``. Helpers take primitive arguments by value and return a
primitive value or ``void``. Supply every argument at the call site. A call can
stand alone as a statement. A ``void`` helper returns at the end of its
block or at a bare ``return``. On x86,
``--target-bits`` selects a 32- or 64-bit object. The output directory must
already exist. A successful write replaces the named file. Source files used
by the compilation are protected from replacement.

Save the following program as ``answer.co``.

.. code-block:: text

   pub fn<int> main () {
       int answer = 42
       answer = 7
       touch()
       return answer
   }

   fn<void> touch() {}

Compile the object and link it with the platform C driver. The resulting
program exits with status 7.

.. code-block:: console

   cloth --kind executable --src answer.co --emit-object answer.o
   cc answer.o -o answer

With MinGW, link with ``gcc answer.o -o answer.exe``.

The built-in ``println`` accepts a signed 32-bit ``int`` for native output.
It writes the decimal value and a newline to standard output. The following
program prints one line containing ``1`` and exits with status 42.

.. code-block:: text

   pub fn<int> main() {
       println(1)
       return 42
   }

Save it as ``print.co``. From the repository root, link the runtime helper
with the emitted object using a C compiler for the same target and width.

.. code-block:: console

   cloth --kind executable --src print.co --emit-object print.o
   cc print.o Runtime/src/println.c -o print

The helper uses the C calling convention and has the signature
``void cloth_rt_println_i32(int32_t value)`` on both target widths.

To call a function in another file, place ``main.co`` and ``helpers.co`` in
the same directory. Import the function in ``main.co``.

.. code-block:: text

   using helpers: identity

   pub fn<int> main () = identity(42)

Declare the function public in ``helpers.co``.

.. code-block:: text

   pub fn<int> identity(int value) = value

Compile the entry file to emit ``main`` and the functions it calls into one
object. In this executable object, ``main`` has external linkage and the
imported helper has local linkage. The linked program exits with status 42.

.. code-block:: console

   cloth --kind executable --src main.co --emit-object main.o
   cc main.o -o main

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
