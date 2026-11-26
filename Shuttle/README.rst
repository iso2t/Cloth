.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Building Cloth projects with Shuttle
====================================

Run ``shuttle build`` in a directory with a ``build.toml`` to compile and
link a Cloth program. Shuttle prints the path of the finished executable.
The program below returns exit status 42.

.. code-block:: toml

   [project]
   name = "answer"
   entry = "main.co"

.. code-block:: text

   pub fn<int> main() = 42;

From a Cloth checkout, build the compiler and its runtime with ``make``
or ``mingw32-make``. Build Shuttle with
``cargo build --release --locked --manifest-path Shuttle/Cargo.toml``.
Put the compiler on ``PATH`` or pass its path explicitly. From the repository
root, use the compiler and Stdlib in this checkout to build ``examples/answer``.

.. code-block:: sh

   cargo run --locked --manifest-path Shuttle/Cargo.toml -- build --manifest-path examples/answer/build.toml --compiler build/cloth --stdlib Stdlib

Project entries and output
--------------------------

Paths in the manifest are relative to its directory. ``project.entry``
takes a Cloth source path. A project using module roots can instead set
``entryPoint = "App.tools.sample.main"`` with ``App = "src"`` under
``[modules]``. Shuttle then compiles ``src/tools/sample.co`` as the
executable entry. For a library the dotted path ends at the source unit.

The output is ``build/<target>/<name>``, with ``.exe`` on Windows or
``.o`` for a library. Set ``directory`` under ``[output]`` to change
the build directory. A failed compile or link preserves the last
successful build. Intermediate objects belong to the current invocation.

Libraries and dependencies
--------------------------

A library manifest names the source unit that supplies its exports.
The module name defaults to the project name and must be a Cloth
identifier. Use ``module`` when the artifact has a different name.

.. code-block:: toml

   [project]
   name = "arithmetic-lib"
   module = "Arithmetic"
   kind = "library"
   entry = "api.co"

A consumer can declare the library by its module name and import its
public functions with ``using Arithmetic : pick``. The dependency path
can name a project directory or its manifest.

.. code-block:: toml

   [dependencies]
   Arithmetic = "../arithmetic"

Shuttle compiles each reachable source dependency into a separate
object on each executable build. Changes to an imported helper take
effect when you rebuild the consumer. To link an existing library,
first run ``shuttle build`` in the library project for the same target,
then use this dependency entry in the consumer.

.. code-block:: toml

   [dependencies]
   Arithmetic = { project = "../arithmetic", prebuilt = true }

Keep the library's manifest, entry source and its imports alongside
the matching object. Cloth uses those sources for visibility, overload
selection and generic specialization. Rebuild the library to publish
an implementation change. For the exported signature types and direct
compiler commands, see `Native libraries <../Documentation/compiler/driver/Objects.rst#native-libraries>`_.

Target and tool selection
-------------------------

``--target-bits 32`` or ``--target-bits 64`` selects an x86 width.
Shuttle queries ``cloth --print-target`` at that width and links
``lib/cloth/<target>/libcloth_runtime.a`` beside the compiler. A missing
archive reports the requested triple and the path searched. Build a
second runtime with ``make runtime RUNTIME_TARGET_BITS=32`` or the
corresponding MinGW Make command. The C toolchain must supply libraries
for that width. On i686 Linux, conversions between floats and integers
wider than 64 bits can require 128-bit compiler builtins that the system
``libgcc`` omits. Use a linker driver with those builtins, or a ``--cc``
wrapper that links a matching compiler runtime. Set ``RUNTIME_CC`` and
``RUNTIME_AR`` to choose the compiler and archiver that build the archive.

Build options take precedence over the corresponding environment variables.

.. list-table::
   :header-rows: 1

   * - Option
     - Environment
     - Value
   * - ``--compiler``
     - ``CLOTHC``
     - Cloth compiler executable. The default is ``cloth`` on ``PATH`` or beside Shuttle.
   * - ``--cc``
     - ``CC``
     - GCC-compatible C linker driver for the target. Defaults to ``gcc`` on Windows and ``cc`` elsewhere.
   * - ``--runtime-dir``
     - ``CLOTH_RUNTIME_DIR``
     - Directory containing the per-triple runtime directories.
   * - ``--stdlib``
     - ``CLOTH_STDLIB``
     - Source root for imports such as ``cloth.collections.List``.
