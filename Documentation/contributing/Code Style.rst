.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Code Style
==========

Follow this guide when adding or editing C++ code for the Cloth
compiler.

Explicit types
--------------

Use an explicit type anywhere a C++ declaration could use ``auto`` or
``decltype(auto)``. This includes local variables, range-based loop
variables, lambda parameters, and named function return types. Use a
named type alias when it makes a declaration easier to read.

Pass a lambda directly to the function that uses it. When a callable
needs a name, use a named function or a callable with an explicit type.

Names
-----

* Use ``snake_case`` for C++ filenames, such as ``source_file.cpp`` and
  ``source_file.hpp``. Use ``PascalCase`` for classes, structs, enum
  types, and type aliases.
* Use ``lowerCamelCase`` for new functions and methods, as in
  ``scanTokens``. Follow the established pattern when extending an API
  family such as the ``get_*`` accessors or ``to_string`` overloads.
* Name function and lambda parameters by role alone in
  ``lowerCamelCase``, as in ``sourcePath``, ``offset``, and ``output``.
* Use ``PascalCase`` for scoped enumerators, as in
  ``TokenType::EndOfFile``. Follow an existing enum's naming pattern
  when adding to it.
* Use ``UPPER_SNAKE_CASE`` for macros and namespace-scope constants,
  as in ``LEXED_ONLY`` and ``V_KEYWORDS``.

Type prefixes
-------------

Name each local variable with a short prefix for its type, an underscore,
and a descriptive ``lowerCamelCase`` name. Apply this convention to loop
variables, local constants, and class data members such as ``s_source``
and ``v_tokens``. Fields in simple value records can use names such as
``Position::line`` and ``Position::column``.

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

	std::size_t countNewlines(std::string_view source) {
		std::size_t st_count = 0;
		for (const char c_character : source) {
			if (c_character == '\n') {
				++st_count;
			}
		}
		return st_count;
	}

	}

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
