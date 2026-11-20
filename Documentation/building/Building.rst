.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

How To Build
============

The native compiler uses GNU Make, a C++20 compiler and a C compiler for
its bundled libraries. Run these commands from the Cloth repository root.

On Linux and macOS, use GNU Make.

.. code-block:: console

   make
   make run

On Windows, use MinGW Make.

.. code-block:: console

   mingw32-make
   mingw32-make run

The default target builds ``build/cloth`` or ``build/cloth.exe``. The
``run`` target is a frontend smoke check. It checks ``Stdlib/cloth/Library.co``
and its imports in library mode, using ``Stdlib`` as the standard library
root. Success is silent apart from Make's command output.

The `Stdlib guide <../../Stdlib/README.md#validate-the-library-sources>`_
also gives the command for checking every shipped module at both target
widths. The `testing guide <../contributing/Testing.rst>`_ lists the
tools required by the test suite.

Use angle brackets for project headers, such as
``#include <lexer/lexer.hpp>``. Make and clangd both read
``compile_flags.txt`` for the C++ standard and include paths. Use ``CC``,
``CXX`` and ``BUILD_DIR`` to select compilers and an output directory.
