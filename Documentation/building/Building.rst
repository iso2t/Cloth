.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

How To Build
============

Cloth uses a C++20 compiler and GNU Make. The Kbuild-style object list is in
``Compiler/Kbuild``.

On Windows with MinGW, run these commands from the repository root.

.. code-block:: console

   mingw32-make
   mingw32-make run

On Linux and macOS, use ``make`` for the same targets. To build with Clang
in its own output directory, pass ``CXX=clang++ BUILD_DIR=build/clang``.

Use angle brackets for project headers, such as
``#include <lexer/Lexer.hpp>``. Make and clangd both read
``compile_flags.txt``, which sets the include root to ``Compiler/include``.

Cloth does have a dependency on LLVM for code emissions.
