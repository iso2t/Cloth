.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Source files and literals
=========================

A Cloth source file contains header imports followed by function and type
declarations. For example, this file defines a function that formats a count.

.. code-block:: text

   fn<string> label(int count) = #"count = {count}"

Save it as ``label.co`` and run ``cloth --src label.co`` to check it. A file
containing only declarations can be checked on its own. The compiler also
reads ``.clib`` files. For an executable entry point, see `Program kind
<../../compiler/Flags.rst#program-kind>`_.

Names and statement boundaries
------------------------------

Source text is UTF-8. Names are case-sensitive and can contain Unicode
letters, combining marks and digits according to the `identifier rules
<Visibility.rst>`_. A lone ``_`` discards a result or stands for a type argument
to infer. Words such as ``fn``, ``pub`` and ``return`` are reserved. ``range``,
``auto`` and ``intrinsic`` have special meanings in particular declarations or
expressions and can also be ordinary identifiers.

Inside a block, separate statements with a newline or semicolon. The last
statement can end at the closing brace. At file scope, a brace-bodied
function or type declaration can end at a newline or the end of the file.
An expression-bodied function also accepts a semicolon. Parentheses,
brackets and comma-separated argument lists can span lines.

``//`` starts a comment that ends at the newline. A block comment starts
with ``/*`` and ends at the first ``*/``. A newline inside a block comment
does not separate statements. A ``using`` import must keep its module path
and colon on the same line.

Numbers
-------

Write ``42`` in decimal, ``0b101010`` in binary, ``0o52`` in octal or ``0x2a``
in hexadecimal. Use underscores between digits, as in ``65_535``. Write
a minus sign before the literal to negate it. Decimal digits can include
Unicode decimal digits. Prefixed bases use ASCII digits and hexadecimal
letters.

A decimal point followed by digits makes a floating literal, as in ``1.25``.
A decimal suffix ``f`` or ``F`` selects floating point, and ``i`` or ``I`` marks an
integer. For example, ``42f`` is floating point and ``42i`` is an integer.
A literal takes its width from its numeric context when it fits. See
`Primitive and nullable types <../types/Types.rst#numeric-types>`_.

Characters and strings
----------------------

Single quotes enclose one Unicode scalar, as in ``'λ'``. Double quotes enclose
a string on one line. In either form, ``\n``, ``\r``, ``\t`` and ``\0`` represent
newline, carriage return, tab and NUL. Use ``\\`` for a backslash, ``\"`` for
a double quote and ``\'`` for a single quote.

Triple double quotes enclose raw text, including newlines and backslashes.
Thus ``"""a\nb"""`` contains a backslash followed by the letter ``n``.
An interpolated string starts with ``#"``. Each pair of braces encloses a
Cloth expression. Double a brace in the surrounding text to include it
literally. In ``#"{{{count}}}"``, the formatted count has a brace on each
side. For formatting a user-defined value, see `Data providers
<../expressions/Access.rst#data-providers>`_.

The `lexer examples <../../../Tests/Compiler/Lexer/Unit/token_test.cpp>`_
cover escapes and interpolation delimiters. To inspect a file's tokens
and source positions, use `lexer mode <../../compiler/Flags.rst#lexer-modes>`_.
