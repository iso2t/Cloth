.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Built-in functions and methods
==============================

``println`` writes a line to standard output. ``exit`` ends the process.
This program prints ``1`` and exits with status 7.

.. code-block:: text

   pub fn<int> main() {
       println(1)
       exit(7)
   }

Printing a line
---------------

The source checker accepts ``println()`` or one positional value that
can be formatted as text. For native output, pass one signed 32-bit
``int``. Its decimal representation is followed by a newline. To link
and run a program using ``println``, see `Run your first Cloth program
<../../getting-started/FirstProgram.rst>`_.

Text input and output
---------------------

``print(value)`` takes one value with a text representation.
For either printing function, a class or struct can supply its text with
``operator ::str() -> string``. ``input()`` returns ``string?`` and accepts an
optional ``string?`` prompt. These functions take positional arguments.
A function declared under the same name takes precedence over the built-in
overloads.

Ending the process
------------------

``exit(status)`` evaluates its integer argument once, converts it to a
signed 32-bit ``int`` on either target width, and flushes buffered output.
Calling it inside a helper ends the whole process. Windows exposes the
32-bit exit status. Unix exposes its low eight bits.

The source checker also accepts a second, positional ``string`` message.
Use the single-argument form for native output. An ordinary function
declared with the name ``exit`` takes precedence over the built-in and
can return to its caller. See the `process exit examples
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

For a call on a nullable receiver, see `Member access
<Access.rst>`_. The `built-in call tests
<../../../Tests/Compiler/Semantic/Unit/builtin_calls_test.cpp>`_ exercise
the source signatures. For the forms that generate native code, see
`Source checks and native code
<../../compiler/driver/Objects.rst#source-checks-and-native-code>`_.
