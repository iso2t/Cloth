.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Built-in functions and methods
==============================

``println`` writes a line to standard output. ``exit`` ends the process.
This program prints ``1`` and exits with status 7.

.. code-block:: text

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

``print(value)`` takes one value with a text representation.
For either printing function, a class or struct can supply its text with
``operator ::str() -> string``. ``input()`` returns ``string?`` and accepts an
optional ``string?`` prompt. The prompt is flushed before reading a line.
End of input and malformed UTF-8 yield ``null``. A blank input line yields
an empty string. These functions take positional arguments.
A function declared under the same name takes precedence over the built-in
overloads.

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

Character and string methods
----------------------------

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
* ``replace(old, new)`` and ``replaceFirst(old, new)`` take two strings.
  ``removePrefix(text)`` and ``removeSuffix(text)`` take one.
  These operations and ``trim()``, ``trimStart()``, and ``trimEnd()`` return strings.

Indices and substring bounds count Unicode scalar values. The substring
includes the start and stops before the end. A missing search result is
``null``. Case conversion uses Unicode simple case mappings.

For a call on a nullable receiver, see `Member access
<Access.rst>`_. The `built-in call tests
<../../../Tests/Compiler/Semantic/Unit/builtin_calls_test.cpp>`_ exercise
the source signatures. For the forms that generate native code, see
`Source checks and native code
<../../compiler/driver/Objects.rst#source-checks-and-native-code>`_.
