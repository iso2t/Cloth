.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Run your first Cloth program
============================

From the repository root, follow the `build instructions
<../building/Building.rst>`_.

Save this program as ``build/hello.co``. A copy is available in the
`println example <../../Tests/Compiler/Driver/Inputs/println.co>`_.

.. code-block:: text

   pub fn<int> main() {
       println(1);
       return 42;
   }

Execution starts at ``main``. The ``int`` in ``fn<int>`` is its return
type. ``println`` writes its argument in decimal followed by a newline.

The first Cloth command below checks the source. The second writes an
object file. Link that object with ``Runtime/src/println.c`` to supply
the print function, then run the executable.

On Linux
--------

.. code-block:: sh

   ./build/cloth --kind executable --src build/hello.co
   ./build/cloth --kind executable --src build/hello.co --emit-object build/hello.o
   cc build/hello.o Runtime/src/println.c -o build/hello
   ./build/hello
   echo $?

On Windows with MinGW
---------------------

In PowerShell, run the following commands.

.. code-block:: powershell

   .\build\cloth.exe --kind executable --src build/hello.co
   .\build\cloth.exe --kind executable --src build/hello.co --emit-object build/hello.o
   gcc build/hello.o Runtime/src/println.c -o build/hello.exe
   .\build\hello.exe
   $LASTEXITCODE

In either shell, the program prints ``1`` on its own line. The last
command displays ``42``, the exit status returned by ``main``.

To call a function defined in a second source file, see `Functions in
another file <../compiler/driver/Objects.rst#functions-in-another-file>`_.
