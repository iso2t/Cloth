.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Console functions, exit, and primitive methods
==============================================

``println`` writes a line to standard output. ``exit`` ends the process.
This program prints ``1`` and exits with status 7.

.. code-block:: text

   using cloth.Console : println;

   pub fn<int> main() {
       println(1);
       exit(7);
   }

Printing a line
---------------

``println()`` writes a newline. With one positional argument it writes the
value's text followed by a newline. For example, ``println(true)`` writes
``true``, and ``println([40, 2])`` writes ``[40, 2]``. To link and run
a program using ``println``, see `Run your first Cloth program
<../../getting-started/FirstProgram.rst>`_.

Text input and output
---------------------

Import the console functions each module uses. For example,
``using cloth.Console : print, input;`` brings those two functions into
scope. The console functions take positional arguments.

``print(value)`` writes the value's text. ``println(value)`` also writes a
newline. Both use the same ``::str`` formatter as string interpolation.
For a null argument, ``print`` writes an empty string and ``println`` writes
just the newline. Struct arguments follow Cloth's usual value semantics.
A formatter that updates a struct field changes the copy passed to the
console function.

``input(prompt)`` accepts any printable value, formats it once, and flushes
the prompt before reading a line. For example, ``input(42)`` displays ``42``.
``input()`` and ``input(null)`` read silently. The result is ``string?``.
When reading from a file or pipe, LF and CRLF end a line. An embedded
Ctrl-Z (``U+001A``) is part of the text. A final line without a newline is
returned before the next call reports end of input.
A blank line yields ``""``. End of input and malformed UTF-8 yield ``null``.

Ending the process
------------------

``exit(status)`` evaluates its integer argument once, converts it to a
signed 32-bit ``int`` on either target width, and flushes buffered output.
Calling it inside a helper ends the whole process. Windows exposes the
32-bit exit status. Unix exposes its low eight bits.

In ``exit(status, message)``, the status evaluates first, then the
positional ``string`` message. Each argument runs once. If either
argument returns from the enclosing function or calls ``exit`` itself,
execution follows that transfer immediately. After both arguments
produce values, the call writes the message's UTF-8 bytes to standard
output and exits. An embedded null byte is part of the output. To end
the message with a newline, include one in the string. An empty message
writes zero bytes. If the output stream reports a write or flush error,
the runtime reports it on standard error and exits with status 1.

An ordinary function declared with the name ``exit`` takes precedence over
the built-in and can return to its caller. See the `message example
<../../../Tests/Compiler/Driver/Inputs/process-exit-message.co>`_ and `process exit tests
<../../../Tests/Compiler/Driver/process-exit.test>`_. The `object-file guide
<../../compiler/driver/Objects.rst#runtime-dependencies>`_ lists the runtime
sources for linking.

Character and boolean conversion
--------------------------------

Call ``toInt()`` on a ``char`` to get its Unicode code point. On a
``bool``, the method returns 1 for ``true`` and 0 for ``false``.

Numeric methods
---------------

``abs()`` and ``clamp(low, high)`` return the receiver's numeric type.
``signum()`` returns an ``int``. The signed minimum has no positive absolute
value at the same width. A lower clamp bound greater than its upper bound
is invalid. For floats, NaN is invalid as a clamp bound or as the receiver
of ``signum()``.

On an integer, ``leadingZeros()`` and ``trailingZeros()`` return a ``usize``
count at the integer's declared width. ``toStringRadix(base)`` takes an
``int`` from 2 through 36 and returns a ``string``.

.. _standard-library-integer-methods:

Standard-library numeric methods
--------------------------------

With the standard library selected, ``a.smaller(b)`` returns the lesser of
two integers of the receiver's type. For example, on an ``unsigned int<65>``
the comparison is unsigned and the result has all 65 bits. The same methods
are available on ``isize`` and ``usize``.

The compiler loads ``Integer`` and ``Float`` from ``cloth/lang/num/Number.co``
under the selected standard library root. The methods and formatters
declared by these bindings are available on numeric receivers without
an import. Inside either declaration, ``self``
is the receiver value and ``Self`` is its exact type, including width and
integer signedness.
For example, these members of ``Integer`` use both forms of method body.

.. code-block:: text

   pub typedef<intrinsic "primitive.integer"> Integer : Number {
       pub mt<Self> smaller(Self other) = if (self < other) self else other;
       pub mt<string> toStringRadix(int radix) = intrinsic "integer.to_string_radix";
       pub operator ::str() -> string = self.toStringRadix(10);
   }

``Float`` binds ``"primitive.float"`` for 16-, 32-, and 64-bit receivers.
Its ``clamp`` and ``signum`` are written in Cloth. Its ``abs`` uses the
native floating-point absolute value, which turns negative zero into
positive zero. For both families, ``value::str``, printing, and string
interpolation use the family's formatter. See the `numeric declarations
<../../../Stdlib/StandardLibrary/src/cloth/lang/num/Number.co>`_ and the
`native examples <../../../Tests/Compiler/Driver/Inputs/numeric-methods.co>`_.

Character and string methods
----------------------------

The compiler loads ``cloth/lang/Primitive.co`` from the selected standard
library. Its ``String``, ``Char``, and ``Bool`` declarations provide methods
on ``string``, ``char``, and ``bool`` without an import. Each declaration's
``Self`` denotes its primitive type. Printing, interpolation, and explicit
``value::str`` require the formatter declared on that type.

A ``char`` supplies the boolean tests ``isWhitespace()``, ``isLetter()``,
``isDigit()``, ``isLetterOrDigit()``, ``isUpperCase()``, and ``isLowerCase()``.
Both ``char`` and ``string`` supply ``toUpperCase()`` and ``toLowerCase()``,
returning the same type as the receiver.

* ``isEmpty()``, ``isBlank()``, ``contains(text)``, ``startsWith(text)``,
  and ``endsWith(text)`` return ``bool``. The text argument is a ``string``.
* ``charAt(index)`` takes a ``usize`` and returns a ``char``.
  ``substring(a, b)`` takes two ``usize`` arguments and returns a ``string``.
  ``indexOf(text)`` and ``lastIndexOf(text)`` return an optional index, ``usize?``.
* ``toChars()`` returns ``char[]``. ``split(separator)`` accepts a ``char`` or a
  ``string`` and returns ``string[]``.
* ``replace(old, new)`` replaces non-overlapping matches from left to right.
  When ``old`` does not occur, it returns the original string without
  allocating. ``replaceFirst(old, new)`` replaces the first match.
  Both methods take two strings and return a string.
* ``removePrefix(text)`` and ``removeSuffix(text)`` each take and return a string.
* ``trimStart()`` removes leading whitespace, ``trimEnd()`` removes trailing
  whitespace, and ``trim()`` removes both. They use the same Unicode whitespace
  test as ``isWhitespace()`` on a ``char``. In native code, a nonempty result
  shares the original string's UTF-8 storage.

Indices and substring bounds count Unicode scalar values. The substring
includes the start and stops before the end. A missing search result is
``null``. Case conversion uses Unicode simple case mappings.
An empty separator splits before the first scalar, between adjacent
scalars, and after the last. With an empty search string, ``replace()``
inserts the replacement at each of those positions and ``replaceFirst()``
inserts it at the beginning. See the `Cloth implementations
<../../../Stdlib/StandardLibrary/src/cloth/lang/Primitive.co>`_ and
`native examples <../../../Tests/Compiler/Driver/Inputs/primitive-text.co>`_.

For a call on a nullable receiver, see `Member access
<Access.rst>`_. The `built-in call tests
<../../../Tests/Compiler/Semantic/Unit/builtin_calls_test.cpp>`_ exercise
the source signatures. For the forms that generate native code, see
`Source checks and native code
<../../compiler/driver/Objects.rst#source-checks-and-native-code>`_.
