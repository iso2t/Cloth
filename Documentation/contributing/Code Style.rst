.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Code Style
==========

Follow this guide when adding or editing C++ code for the Cloth compiler.
The repository's `.clang-tidy <../../.clang-tidy>`_ defines the checks.

Cloth keywords and operators
----------------------------

Keep the spelling of each Cloth keyword and operator in the token module.
Use the ``Keyword`` and ``Operator`` enumerators when making decisions about
syntax. When matching or emitting source text, obtain the spelling with
``toString``. Use ``toStringmulti`` for a known, space-separated sequence of
keywords. Apply the same rule to synthesized tokens, type names, and
diagnostics that show the programmer what to write.

Source fixtures and tests that verify the public spelling should contain
literal Cloth text so they can catch unintended language changes.

Explicit types
--------------

Prefer explicit types when they make a declaration easier to understand.
Use ``auto`` when an iterator, cast, or ``new`` expression already
establishes the type. Keep pointer and reference markers visible, as in
``auto *node`` and ``const auto &entry``. Use a named type alias when it
improves readability.

Pass short lambdas directly to the function that uses them when the call
stays readable. Give a callable a name when that clarifies its purpose
or lets you reuse it. A named lambda may use ``auto``.

Initialization and iteration
----------------------------

Initialize local variables and use constructor member initializers where
possible. Use designated initializers for multi-field aggregates when the
language permits them. Mark values, references, and member functions
``const`` when they only need read access. Use ``override`` on overrides
and ``nullptr`` for null pointers.

Prefer range-based loops when visiting a whole container. Keep an index
when a traversal must also visit entries it appends, or when it uses the
same position in two sequences. Use a const reference to read an element
that would be expensive to copy. Test for an empty container with
``empty()``.

Names
-----

* Use ``snake_case`` for C++ filenames, such as ``source_file.cpp`` and
  ``source_file.hpp``.
* Use ``PascalCase`` for classes, structs, enum types, type aliases
  (including typedefs), and template parameters.
* Use ``lowerCamelCase`` for functions and methods, including accessors
  and overloads, as in ``scanTokens``, ``getSpan``, and ``toString``.
* Name function and lambda parameters by role alone in
  ``lowerCamelCase``, as in ``sourcePath``, ``offset``, and ``output``.
  Leave an unused parameter unnamed when the signature requires it.
* Use ``PascalCase`` for both scoped and unscoped enumerators, as in
  ``TokenType::EndOfFile``, ``KReturn``, and ``OPlus``.
* Use ``UPPER_SNAKE_CASE`` for macros and constants at global or
  namespace scope, as in ``LEXED_ONLY`` and ``V_KEYWORDS``.

Type prefixes
-------------

Use a descriptive ``lowerCamelCase`` name for locals, loop variables,
local constants, and data members. A short type prefix followed by an
underscore is optional. For example, both ``tokens`` and ``v_tokens`` fit
the convention. When using a prefix, follow the surrounding code.

.. list-table::
   :header-rows: 1

   * - C++ type
     - Prefix
     - Example
   * - ``bool``
     - ``b_``
     - ``b_isKeyword``
   * - ``char`` or ``unsigned char``
     - ``c_``
     - ``c_character``
   * - ``int``
     - ``i_``
     - ``i_currentIndex``
   * - ``std::int64_t``
     - ``i64_``
     - ``i64_target``
   * - ``std::size_t``
     - ``st_``
     - ``st_lexemeWidth``
   * - ``std::string`` or ``std::string_view``
     - ``s_``
     - ``s_lexeme``
   * - ``std::vector<T>``
     - ``v_``
     - ``v_tokens``
   * - ``std::ifstream``
     - ``ifs_``
     - ``ifs_input``

Reuse established prefixes for compiler types, such as ``l_`` for
``Lexer``, ``p_`` for ``Position``, and ``t_`` for ``Token``. A reference
uses the prefix of the referenced type, as in ``const Token &t_token``.
Choose a recognizable abbreviation when a type needs a new prefix.

.. code-block:: cpp

	// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

	#include <cstddef>
	#include <string_view>

	namespace cloth {

	std::size_t countNewlines (std::string_view source) {
		std::size_t st_count = 0;
		for (const char c_character : source) {
			if (c_character == '\n') {
				++st_count;
			}
		}
		return st_count;
	}

	}

Checking a patch
----------------

Run the `repository checker <../../scripts/clang_tidy_patch.py>`_ from the
repository root. It uses the project's compilation database and checks a
changed header through the translation units that include it.

.. code-block:: console

   python scripts/clang_tidy_patch.py

For an on-demand check of the whole project before a push, run
``python scripts/clang_tidy_patch.py --all``. Both commands report findings.
Add ``--fix`` when you want to apply fixes. After a rename, check declarations,
overrides, definitions, and callers together. Run ``make test``
(``mingw32-make test`` on Windows) to build the compiler and exercise its
callers, including tests.

Formatting
----------

Use the repository's ``.clang-format`` for C++ layout. It uses tabs with
an eight-column width, attached opening braces, and a 160-column wrapping
target. Place pointer and reference markers next to the name, as in
``const char *source`` and ``const Token &token``.

Run ClangFormat on the C++ files you change and review the resulting diff.

.. code-block:: console

   clang-format --style=file -i path/to/changed_file.cpp

Save text files as UTF-8 with LF line endings and a final newline.

Headers and includes
--------------------

Put ``#pragma once`` below the license header in new C++ headers.
Include the headers that provide the types and functions you use.
Use angle brackets for Cloth headers and paths relative to
``Compiler/include``, as in ``#include <file/source_file.hpp>``.
Let ClangFormat group and sort the includes.

Comments
--------

Use comments to explain constraints, invariants, and decisions that
are hard to infer from the code. When documenting an interface with
Doxygen, describe the behavior and any inputs, results, or errors
that callers need to understand.

License headers
---------------

Put the SPDX comment on the first line and leave a blank line below it.

* C++ sources and headers use
  ``// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception``.
* Makefiles and configuration files with hash comments use
  ``# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception``.
* reStructuredText pages use
  ``.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception``.

Read the license terms in `LICENSE <../../LICENSE>`_.
