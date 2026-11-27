.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Building Cloth projects with Shuttle
====================================

Use ``shuttle init`` to start a Cloth project and ``shuttle run`` to build and
run it. Shuttle reads the KDL v2 manifest in ``build.k`` and invokes the
Cloth compiler. Use ``shuttle build`` to build a single target and print its
artifact path.

A first executable
------------------

From the Cloth checkout, build the compiler and runtime with ``make`` or
``mingw32-make`` and build Shuttle with
``cargo build --release --locked --manifest-path Shuttle/Cargo.toml``.
Put ``shuttle`` and ``cloth`` on your path or pass the compiler path with
``--compiler``.

.. code-block:: sh

   shuttle init hello
   cd hello
   shuttle run

The program prints ``Hello, world!``.
With no path argument, ``shuttle init`` initializes the current directory.
The directory name supplies the project name. Shuttle creates ``src/main.co``
and the following ``build.k``. If either file already exists, initialization
stops and preserves the existing files.

.. code-block:: text

   project "hello" version="0.1.0" {
       target "executable" entry="src/main.co"
   }

A standalone library can use the same layout. Give the library target
``entry="src/api.co"`` and the kind ``"library"``. Its import name
defaults to the project name. Set ``module="Arithmetic"`` to supply another
Cloth identifier, for example when the project name contains a hyphen.

Run a project
-------------

Shuttle looks for ``build.k`` in the given directory and its parents.
With no path, it starts in the current directory. You can also pass the
manifest file directly. Arguments after ``--`` go directly to the program.
For example, from the Cloth checkout the calculator prints ``42``.

.. code-block:: sh

   shuttle run examples/calculator -- 6 '*' 7

Shuttle builds every declared executable and library and compiles each
included module with an ``api``, including unused APIs. A module that owns
a target's entry file builds with that target. Each executable links its own
``uses`` dependencies. All compilation and linking must succeed before the
program starts.

Shuttle runs the executable that inherits the project name, or the sole
executable when there is only one. To choose among several named executables,
use ``shuttle run --target NAME``. The program starts in the project directory,
inherits standard input, output, and error, and supplies Shuttle's exit status.
``run`` accepts the same manifest and toolchain options as ``build``.

Add a module to a project
-------------------------

Run this command from the project directory or one of its subdirectories.

.. code-block:: sh

   shuttle init --module Arithmetic

Shuttle finds the nearest ``build.k`` and adds ``include "Arithmetic"``.
The new ``Arithmetic/`` directory at the project root contains ``module.k``
and ``src/api.co``. Module names must be available in the project and follow
Cloth's identifier rules. ``init --module``, ``build.k`` and ``module.k`` all
reject reserved keywords such as ``fn`` and the lone underscore ``_``.

Concurrent module additions take turns updating ``build.k``. Shuttle keeps
``.shuttle.lock`` in the project root and holds an operating-system lock
on it during each update. If an addition is interrupted before its include
is published, rerun ``shuttle init --module NAME`` to finish registering it.

The generated API exports ``answer()``. To call it from the executable,
add a ``uses`` declaration to its target in ``build.k``.

.. code-block:: text

   target "executable" entry="src/main.co" {
       uses "Arithmetic"
   }

Use the exported function in ``src/main.co``, then run ``shuttle run``.
The command returns exit status 42.

.. code-block:: text

   using Arithmetic : answer;
   pub fn<int> main() = answer();

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

From the Cloth checkout, build either target from the console module's
source directory.

.. code-block:: sh

   cd examples/calculator/Console/src
   shuttle build
   shuttle build arithmetic-lib

The first build produces the ``calculator`` executable. Run the path it
prints with ``6 '*' 7`` to get ``42``. The console accepts two signed
32-bit integers and one of ``+``, ``-``, ``*``, or ``/``. Division truncates
toward zero. The second build produces ``arithmetic-lib.o`` for other
Cloth projects to link.

``shuttle build`` searches the current directory and its parents for the
nearest ``build.k``. It selects the target that inherits the project name.
When every target has an explicit name, a sole target is the default.
With several named targets, Shuttle lists the choices. To select a project
explicitly, pass ``--manifest-path PATH`` with a manifest file or a directory
containing ``build.k``.

The default output is ``build/<target-triple>/<artifact>``. A project can
add ``output "out"`` to choose another directory. Shuttle finishes compilation
and linking before publishing the selected targets. If publication fails, Shuttle
restores the previous executables and library objects and removes any
outputs the failed build added. If the filesystem prevents a restore,
Shuttle reports the affected path and keeps the backups for recovery.
Concurrent builds use ``.shuttle.lock`` in the output directory to publish
one at a time.

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
specialization.

Before linking a prebuilt, Shuttle checks the native interfaces recorded in
the object against the source checked for this executable. For example,
swapping two ``int`` parameter names changes how a named call passes its
arguments and requires a provider rebuild. Swapping two ``int`` fields in
a returned struct also requires a rebuild because a consumer reads each
field at its checked byte offset. Edits confined to function bodies
leave the existing object usable. If an interface has changed or an older
object lacks a record, rebuild the library target named in the diagnostic
for the same target width. See `Native libraries <../Documentation/compiler/driver/Objects.rst#native-libraries>`_.

The shipped `standard library project <../Stdlib/build.k>`_ can be built
from the checkout with its source root supplied to the native compiler.

.. code-block:: sh

   cargo run --locked --manifest-path Shuttle/Cargo.toml -- build --manifest-path Stdlib/build.k --compiler build/cloth --stdlib Stdlib/StandardLibrary/src

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
