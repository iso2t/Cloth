.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Run your first Cloth program
============================

After `building Cloth <../building/Building.rst>`_, work from the
repository root.

Save this program as ``build/hello.co``. You can also copy the
`example source <../../Tests/Compiler/Driver/Inputs/println.co>`_.

.. code-block:: text

   using cloth.Console : println;

   pub fn main() {
       println("Hello, world!");
   }

The ``using`` line imports ``println`` from ``cloth.Console``. Execution
starts at ``main``, where ``println`` writes the greeting followed by a
newline.

The compiler finds ``cloth.Console`` in its bundled standard library.
The first Cloth command checks the source. The second writes an object
file. Link the object with the runtime archive for your target, then run
the executable.

On Linux
--------

.. code-block:: sh

   ./build/cloth --kind executable --src build/hello.co
   ./build/cloth --kind executable --src build/hello.co --emit-object build/hello.o
   cc build/hello.o "build/lib/cloth/$(./build/cloth --print-target)/libcloth_runtime.a" -o build/hello
   ./build/hello

On Windows with MinGW
---------------------

In PowerShell, run the following commands.

.. code-block:: powershell

   .\build\cloth.exe --kind executable --src build/hello.co
   .\build\cloth.exe --kind executable --src build/hello.co --emit-object build/hello.o
   $target = .\build\cloth.exe --print-target
   gcc build/hello.o "build/lib/cloth/$target/libcloth_runtime.a" -lshell32 -o build/hello.exe
   .\build\hello.exe

You should see one line in your terminal.

.. code-block:: text

   Hello, world!

To call a function defined in a second source file, see `Functions in
another file <../compiler/driver/Objects.rst#functions-in-another-file>`_.
