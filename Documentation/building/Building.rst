.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

How To Build
============

The native compiler uses GNU Make, a C++20 compiler, a C compiler for its
bundled libraries, and the LLVM development headers and libraries with
native target support. Make queries ``llvm-config`` from ``PATH``.
Set ``LLVM_CONFIG`` to select another installation. On Ubuntu 24.04,
install ``llvm-18-dev`` and export ``LLVM_CONFIG=llvm-config-18``.
Use a separate ``BUILD_DIR`` for each LLVM installation when switching
between them.

On Windows, the MSYS2 UCRT64 package ``mingw-w64-ucrt-x86_64-llvm``
supplies the development files. MinGW also supports an MSVC-built LLVM
SDK with ``LLVM-C.lib``. Keep the selected LLVM installation's binary
directory on ``PATH`` so Windows can load its DLL.

Run these commands from the Cloth repository root.

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

With the compiler built, `write and run your first Cloth program
<../getting-started/FirstProgram.rst>`_.

The `Stdlib guide <../../Stdlib/README.md#validate-the-library-sources>`_
also gives the command for checking every shipped module at both target
widths. The `testing guide <../contributing/Testing.rst>`_ lists the
tools required by the test suite.

Use angle brackets for project headers, such as
``#include <lexer/lexer.hpp>``. Make and clangd both read
``compile_flags.txt`` for the C++ standard and include paths. Use ``CC``,
``CXX`` and ``BUILD_DIR`` to select compilers and an output directory.
