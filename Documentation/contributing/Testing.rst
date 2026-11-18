.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Running Cloth tests
===================

Run these GNU Make targets from the repository root. Use ``mingw32-make``
on Windows with MinGW. LLVM lit runs source-file and CLI regressions.
GoogleTest checks compiler data structures through their C++ interfaces.

.. list-table::
   :header-rows: 1

   * - Scope
     - Command
   * - One fixture
     - ``make test-file CASE=Tests/Compiler/Driver/source-path.test``
   * - One C++ test
     - ``make test-unit CASE=ParserPrecedence.PowerBindsBeforeUnaryMinus``
   * - Driver
     - ``make test-driver``
   * - Lexer
     - ``make test-lexer``
   * - Parser
     - ``make test-parser``
   * - Diagnostics
     - ``make test-diagnostics``
   * - All C++ tests
     - ``make test-unit``
   * - Whole suite
     - ``make test``

The lexer and diagnostics targets run both kinds of tests. The driver
target runs CLI fixtures. The parser target runs C++ assertions on the
syntax tree. Pass ``-k`` to Make to continue running independent suites
after a failure.

Test dependencies
-----------------

File-based tests require Python 3, LLVM lit (``llvm-lit`` or ``lit``),
and LLVM FileCheck on ``PATH``. Use ``PYTHON``, ``LIT``, and ``FILECHECK``
to select specific executables. Each lit run uses a private directory
under ``BUILD_DIR/Tests/``, which defaults to ``build/Tests/``. A failed
run prints that directory and keeps its files. Successful runs clean it up.

The first native test build uses Python to fetch GoogleTest 1.18.0,
verifies its SHA-256 digest, and caches the source under
``BUILD_DIR/Tests/deps/``. For an offline build, set ``GTEST_ARCHIVE`` to
a local copy of the official release tarball. Make compiles GoogleTest
with Cloth's C++ compiler and flags and links the test executable against
the production objects, excluding the CLI entry point. An ordinary
``make`` builds Cloth independently of these test dependencies.
See the `GoogleTest release <https://github.com/google/googletest/releases/tag/v1.18.0>`_.

Adding a C++ test
-----------------

Put a ``.cpp`` file in ``Unit/`` under the owning area and list it in
``unit_sources`` in ``Tests/Makefile``. Start the GoogleTest suite name
with ``Lexer``, ``Parser``, or ``Diagnostics`` so the area target selects
it. For example, ``ParserPrecedence.PowerBindsBeforeUnaryMinus`` parses
``-2 ** 2`` and checks that unary minus wraps the power expression.

Use the reported ``Suite.Case`` name with ``make test-unit CASE=...`` to
rerun a failure. GoogleTest filters such as ``CASE=ParserPrecedence.*``
also work. A filter that selects zero tests exits with an error. Run
``build/Tests/cloth-unit --gtest_list_tests`` to list names after a build
(use ``cloth-unit.exe`` on Windows).

Adding a fixture
----------------

Place a test in the directory for the behavior it checks. Lit discovers
``.co``, ``.clib``, and ``.test`` files. Write ``// RUN:`` in Cloth source
and ``# RUN:`` in a ``.test`` script. Put auxiliary source files in an
adjacent ``Inputs/`` directory. Lit discovers new fixtures automatically
under its registered areas. When adding the first fixture to a native-only
area, register that directory in ``Tests/Makefile`` for the area target.

Use ``%cloth`` for the selected compiler, ``%filecheck`` to check output,
and ``%t`` for per-test scratch files. Quote paths in directives so they
work in checkouts containing spaces. The following directive accepts a
source file only when the compiler exits successfully with both streams
empty.

.. code-block:: text

   // RUN: %expect_exit --empty-stdout --empty-stderr 0 %cloth --src "%/s"

Pass the compiler's full status constant to ``%expect_exit``. The helper
handles the low eight bits visible on POSIX. For example, silent lexing
success and a compiler error both produce status ``1`` on Linux. Check
the expected streams or diagnostic along with that status.
