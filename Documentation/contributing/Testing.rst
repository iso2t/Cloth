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

Continuous integration
----------------------

On each push and pull request, GitHub Actions runs the whole suite on
Ubuntu 24.04 with GCC and Windows Server 2025 with MinGW. Each job builds
the compiler and unit-test executable with ``make -j2 test-build``, then
runs ``make -k test``. Ubuntu also builds the compiler in ``build/ubsan``
with UndefinedBehaviorSanitizer enabled for C and C++ and reruns the CLI
fixtures. A sanitizer finding fails the fixture even when it expects a
Cloth diagnostic. Lit uses two workers and a 60-second timeout per fixture.
A failure on either host leaves the other job running.

The job summary shows each runner's executed count, elapsed time, result
codes, and slowest case. A missing report or a runner with zero executed
cases fails the job. The ``test-results-<OS>-<attempt>`` artifact retains
the build and test logs, lit JSON and xUnit results, GoogleTest XML, and
any failed lit run directories for 14 days. To rerun a lit failure, prefix
the path after ``Cloth ::`` with ``Tests/`` and pass it to ``test-file``.
Use GoogleTest's ``Suite.Case`` name with ``test-unit``.
See the `test workflow <../../.github/workflows/tests.yml>`_.

Checking shard coverage
~~~~~~~~~~~~~~~~~~~~~~~

CI runs the full set of fixtures on each host. To try two shards locally,
use the same compiler build, suite, and lexical order for all three runs.
The shard commands can run concurrently after the build finishes.

.. code-block:: console

   make test-build
   make lit-all LIT_OPTS="--order=lexical -o build/Tests/all.json"
   make lit-all LIT_OPTS="--order=lexical --num-shards=2 --run-shard=1 -o build/Tests/part-1.json"
   make lit-all LIT_OPTS="--order=lexical --num-shards=2 --run-shard=2 -o build/Tests/part-2.json"

Compare the ``name`` fields under ``tests`` in the JSON reports. Every
name in the complete run should occur exactly once across the two shard
reports. Each lit invocation creates its own scratch directory.

To include native tests in the trial, use ``make test`` and set
``GTEST_TOTAL_SHARDS=2`` on both workers. Pair lit shard 1 with
``GTEST_SHARD_INDEX=0`` and lit shard 2 with ``GTEST_SHARD_INDEX=1``.
Give each run its own ``GTEST_OUTPUT=xml:<path>`` and compare the
``classname`` and ``name`` pairs of executed cases against the unsharded
GoogleTest report.
