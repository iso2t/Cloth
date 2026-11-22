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
executes ``return``, ``break``, ``continue`` or the built-in ``exit``, earlier
side effects are preserved. Execution skips the remaining operands and the
enclosing operation.

Locals and non-generic helper functions can use signed and unsigned integers
of any Cloth width, ``isize``, ``usize``, ``bool``, ``char`` and 16-, 32- or
64-bit floats. A mutable numeric local supports assignment, compound arithmetic
assignment, increment and decrement.
A local declared with ``:=`` keeps its initial value. A ``def`` binding infers
its type from the initializer.

Nullable integers, booleans, floats and characters use a one-byte presence
flag followed by the payload at its usual alignment. On both target widths,
``int?``, ``float?`` and ``char?`` occupy eight bytes. A ``bool?`` occupies two.
A present character stores its full 32-bit Unicode scalar value, including
``'\0'``. Comparisons with ``null`` test the flag. Two absent values compare
equal. Present floats use ordinary floating-point equality at their declared
precision. Positive and negative zero compare equal, and a present NaN
compares unequal to itself.
The ``?:`` operator evaluates its fallback only for an absent value. A present
``false``, ``0.0`` or ``'\0'`` skips it. This program returns 42.

.. code-block:: text

   pub fn<int> main() {
       int? value = 0
       if (value == null || value != 0) return 1
       value = null
       return if (value == null) 42 else 2
   }

An enum stores a four-byte tag on both target widths.
Tags start at zero and follow declaration order, including private cases.
The built-in ``==`` and ``!=`` compare tags for values of the same enum.
Casting a case or a stored enum value to ``int`` gives its tag.
A payload field is read-only. Reading it selects the case's explicit value
or the field's declared default. The object emitter supports integers,
booleans, floats, characters, their nullable forms and structs composed of
supported fields. In this example ``Mode.Off.code`` uses the default of 7
and ``main`` returns 42.

.. code-block:: text

   typedef<enum> Mode(pub def<int> code = 7) { pub Off, pub On(42) }

   pub fn<int> main() {
       Mode mode = Mode.On
       return mode.code
   }

Local structs can contain the supported primitives, their nullable forms,
enums and other structs. A positional literal evaluates its fields once in
declaration order, including nested literals. If a field transfers control out
of the initializer, evaluation stops there. Assigning a struct copies all its
nested fields. Numeric fields of a mutable local also support compound
assignment, increment and decrement. This program returns 7 on both target
widths. The earlier copy still holds 42.

.. code-block:: text

   typedef<struct> Inner { pub bool ready, pub int<12> value }
   typedef<struct> Sample { pub bool tagged, pub Inner inner }

   pub fn<int> main() {
       Sample sample = {true, {true, 42}}
       def original := sample
       sample.inner.value = 7
       return if (sample.tagged && sample.inner.ready && original.inner.value == 42) sample.inner.value -> int else 1
   }

``Inner`` occupies 4 bytes with 2-byte alignment. It is stored inline at
byte 2 of ``Sample``, placing ``sample.inner.value`` at byte 4. ``Sample``
occupies 6 bytes with 2-byte alignment.

Integer arithmetic, including ``**``, wraps at the operand width. On both
targets ``int<12>`` wraps from 2047 to -2048. The width of ``isize`` and
``usize`` follows ``--target-bits``. Unary ``~`` flips every bit at that width.
Signed division truncates toward zero, and a nonzero remainder has the sign
of the dividend. Integer ``/``, ``%``, ``/=`` and ``%=`` evaluate both
operands once, in source order. A zero divisor reports
``Cloth runtime error: integer division by zero``. A signed minimum dividend
with divisor -1 reports ``Cloth runtime error: signed integer division overflow``.
Integer ``**`` with a negative exponent reports
``Cloth runtime error: negative integer exponent``.
The frontend diagnoses literal zero divisors (``C3050``), literal signed division
overflow (``C3051``) and literal negative integer exponents (``C3018``).

Comparisons use the checked operand type. Integer-to-boolean conversion
maps zero to false and every nonzero value to true, including at parameter
and return boundaries. Booleans occupy one byte. Built-in ``&&`` and ``||``
evaluate their right operand only when needed. A character holds one Unicode
scalar value. The ``toInt()`` method returns its code point or a boolean's
normalized integer value.

Numeric casts use the destination width. Floating-to-integer casts truncate
toward zero. If the operand is NaN or infinite, or the truncated integer is
outside the destination range, the cast reports
``Cloth runtime error: invalid float-to-integer cast (NaN or out of range)``.
For example, ``-128.75`` converts to ``-128`` in ``int<8>``, while ``-129.0``
reports the error.

Arithmetic and conversion errors flush earlier standard output, write one
diagnostic to standard error and exit with status 1. Link objects that use
these checks with ``Runtime/src/errors.c``, for example ``cc main.o Runtime/src/errors.c -o main``.

The built-in ``exit(status)`` evaluates its integer argument once and converts
it to a signed 32-bit ``int`` on both target widths. It flushes buffered
output and ends the process. Windows exposes the 32-bit exit status and
Unix exposes its low eight bits. Link objects that call it with ``Runtime/src/process.c``.

Floating comparison follows IEEE rules, including unordered inequality for
NaN. Floating remainder and exponentiation can require the platform math
library. On Unix, link these objects with ``-lm``.

A call can reach a function in the same file or a ``pub`` function imported
with ``using``. Helpers take the supported primitives, their nullable forms
and enums by value. They can return one such value or ``void``. Supply every
argument at the call site. A call can stand alone as a statement. A ``void``
helper returns at the end of its block or at a bare ``return``. On x86,
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
