.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Run your first Cloth program
============================

Start with ``shuttle`` and ``cloth`` on your ``PATH``. The
`Shuttle setup instructions <../../Shuttle/README.rst#a-first-executable>`_
explain how to build both tools.

1. Run Hello World
------------------

From the directory where you keep your projects, create a project named
``hello`` and run it. The first command creates the project files. The
second builds the program and starts it.

.. code-block:: sh

   shuttle init hello
   shuttle run hello

The program prints one line.

.. code-block:: text

   Hello, world!

Open ``hello/src/main.co`` to see the program Shuttle created.

.. code-block:: text

   using cloth.Console : println;

   pub fn main() {
       println("Hello, world!");
   }

``using cloth.Console : println;``
   ``using`` imports names into this file. Write the module path
   (``cloth.Console``) before ``:`` and the public names you need
   (``println``) after it. Put imports before declarations and end each
   one with a semicolon.

``pub fn main() {``
   ``fn`` declares a function, and ``pub`` makes it visible to other
   files. Cloth calls ``main`` when the program starts. The empty
   parentheses declare that it takes no parameters. The opening brace
   begins its body.

``println("Hello, world!");``
   Double quotes enclose a string literal. The parentheses pass that
   string to ``println``, which writes the text followed by a newline.
   The semicolon ends the call statement.

``}``
   The closing brace ends the function body. This ``main`` has no declared
   result, so reaching the closing brace finishes the program successfully.

Try adding ``println("Goodbye!");`` just below the first call, inside the
braces. Predict where the second message will appear, then run
``shuttle run hello`` to check. Remove the extra call before continuing.

2. Greet someone by name
------------------------

Read a name and use it in the greeting
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Make three edits in ``hello/src/main.co``. Keep the original ``main``
declaration for this first run.

``using cloth.Console : println, input;``
   Add ``input`` after ``println``. A comma separates names imported from
   the same module. Both functions are now available inside ``main``.

``string name = input("What is your name? ") ?: "";``
   Insert this line above the greeting. A local variable declaration puts
   the type (``string``) before the name (``name``). The ``=`` initializes
   this variable with the value of the expression on its right.

   ``input`` displays the prompt and reads one line. Its result type is
   ``string?``, which can hold text or ``null``. In ``a ?: b``, Cloth keeps
   ``a`` when it has a value and uses ``b`` when ``a`` is ``null``.
   Pressing Enter on a blank line gives an empty string directly. If
   input ends before a line is read, the fallback supplies ``""``.

``println(#"Hello {name}!");``
   Replace the original ``println`` call. The ``#"`` opens an interpolated
   string. The braces enclose an expression, here the variable ``name``.
   Cloth inserts its value between the space and the exclamation mark.

Run the project from the same directory where you created it.

.. code-block:: sh

   shuttle run hello

Type ``Ada`` and press Enter.

.. code-block:: text

   What is your name? Ada
   Hello Ada!

What will the greeting say if you press Enter without typing a name?
Run it again to check. The empty string contributes no characters, so
the greeting reads ``Hello !``.

Stop before greeting an empty name
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``pub fn<isize> main() {``
   Change the function header. Angle brackets after ``fn`` declare the
   result type. ``isize`` is a signed, pointer-sized integer. Each path
   that finishes ``main`` must now return an integer. Shuttle passes that
   result to the shell as the program's exit status.

``if (name.isEmpty()) return -1;``
   Insert this check immediately after reading ``name``. The dot selects
   the string's ``isEmpty`` method, and ``()`` calls it with no arguments.
   It returns ``true`` when the string contains zero characters.
   ``if`` tests the condition in parentheses. Its body can be a single
   statement. Here an empty reply runs ``return -1;`` and exits ``main``
   immediately. Otherwise, execution continues at ``println``.

``return 0;``
   Add this statement after ``println`` and before the closing brace. A
   nonempty name reaches this line after the greeting is printed. Zero
   tells the caller that the program succeeded.

Compare your edits with the finished file.

.. code-block:: text

   using cloth.Console : println, input;

   pub fn<isize> main() {
       string name = input("What is your name? ") ?: "";
       if (name.isEmpty()) return -1;

       println(#"Hello {name}!");

       return 0;
   }

Run ``shuttle run hello`` with ``Ada`` and then again with an empty line.
Before each run, predict which ``return`` will execute. You can inspect
the exit status immediately afterward with ``$LASTEXITCODE`` in PowerShell
or ``echo $?`` in a POSIX shell. Look for zero after the greeting and a
nonzero status after the empty reply.
