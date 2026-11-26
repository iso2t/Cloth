.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Building Cloth projects with Shuttle
====================================

Run ``shuttle build`` from a project directory containing ``build.k``.
Shuttle reads KDL v2 manifests, invokes the Cloth compiler, and prints the
path of the finished artifact. A project can keep its own sources in ``src/``.

A first executable
------------------

Create ``build.k`` alongside the ``src/`` directory.

.. code-block:: text

   project "answer" version="0.1.0" {
       target "executable" entry="src/main.co"
   }

Put the program in ``src/main.co``.

.. code-block:: text

   pub fn<int> main() = 42;

From the Cloth checkout, build the compiler and runtime with ``make`` or
``mingw32-make`` and build Shuttle with
``cargo build --release --locked --manifest-path Shuttle/Cargo.toml``.
Put ``shuttle`` and ``cloth`` on your path or pass the compiler path with
``--compiler``. From the example project, ``shuttle build`` builds ``answer``
(``answer.exe`` on Windows). Running it returns exit status 42.

A standalone library can use the same layout. Give the library target
``entry="src/api.co"`` and the kind ``"library"``. Its import name
defaults to the project name. Set ``module="Arithmetic"`` to supply another
Cloth identifier, for example when the project name contains a hyphen.

Build an executable and a library
---------------------------------

The `calculator example <../examples/calculator>`_ includes two sibling
module directories in its ``build.k``. Each directory has a ``module.k``
beside its ``src/``. Project paths are relative to ``build.k``. A module's
API path is relative to its ``module.k``.

.. code-block:: text

   project "calculator" version="0.1.0" {
       include "Console"
       include "Arithmetic"

       target "executable" entry="Console/src/main.co"
       target "library" name="arithmetic-lib" module="Arithmetic"
   }

Declare the dependency in ``Console/module.k``.

.. code-block:: text

   module "Console" {
       uses "Arithmetic"
   }

Give ``Arithmetic/module.k`` the import name and public source file.

.. code-block:: text

   module "Arithmetic" api="src/api.co"

The `console entry <../examples/calculator/Console/src/main.co>`_ imports
``add``, ``subtract``, ``multiply``, and ``divide`` from the
`Arithmetic API <../examples/calculator/Arithmetic/src/api.co>`_.
Shuttle builds the modules reached through ``uses``. To use a module from
a project-owned entry such as ``src/main.co``, put ``uses`` inside that
``target`` node.

To build both targets, start in the Cloth checkout and enter the example directory.

.. code-block:: sh

   cd examples/calculator
   shuttle build
   shuttle build arithmetic-lib

The first build produces the ``calculator`` executable. Run the path it
prints with ``6 '*' 7`` to get ``42``. The console accepts two signed
32-bit integers and one of ``+``, ``-``, ``*``, or ``/``. Division truncates
toward zero. The second build produces ``arithmetic-lib.o`` for other
Cloth projects to link.

``shuttle build`` selects the target that inherits the project name.
When every target has an explicit name, a sole target is the default.
With several named targets, Shuttle lists the choices. Use
``--manifest-path PATH`` to select a project directory or its manifest
from a different working directory.

The default output is ``build/<target-triple>/<artifact>``. A project can
add ``output "out"`` to choose another directory. A failed compile or link
preserves the previous artifact.

Use a library from another project
----------------------------------

Declare the provider in the consumer's ``build.k`` and refer to its import
name from the target or module that uses it.

.. code-block:: text

   project "consumer" {
       dependency "Arithmetic" path="../calculator"
       target "executable" entry="src/main.co" {
           uses "Arithmetic"
       }
   }

Shuttle selects the provider's library target by its Cloth module name.
The path can point to the project directory or its ``build.k``. Each
reachable source library compiles once per executable build. For an
existing object, build its named target for the same machine width and
add ``prebuilt=#true`` to the ``dependency`` node. Keep its manifest,
API source and imports beside the artifact for type checking and generic
specialization. See `Native libraries <../Documentation/compiler/driver/Objects.rst#native-libraries>`_.

The shipped `standard library project <../Stdlib/build.k>`_ can be built
from the checkout with its source root supplied to the native compiler.

.. code-block:: sh

   cargo run --locked --manifest-path Shuttle/Cargo.toml -- build --manifest-path Stdlib/build.k --compiler build/cloth --stdlib Stdlib/StandardLibrary/src

Migrate a TOML manifest
-----------------------

Move the project name and version from ``[project]`` to the ``project`` node
in ``build.k``. Convert its kind and entry into a ``target`` and place the
source under the project's or an included module's ``src/``. Translate a
dotted ``entryPoint`` using its old ``[modules]`` mapping to find the same
source file. For example, the old standard library entry
``StandardLibrary.cloth.Library`` becomes ``src/cloth/Library.co`` in
``StandardLibrary/module.k``. Convert module roots into ``include`` nodes,
dependencies into ``dependency`` and ``uses`` nodes, and ``[output].directory``
into an ``output`` node. Cargo reads ``Shuttle/Cargo.toml`` to build the Rust CLI.

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
     - Override the standard library bundled with the selected compiler.
