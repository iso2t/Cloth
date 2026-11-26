.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Run your first Cloth program
============================

Start with ``shuttle`` and ``cloth`` on your ``PATH``. The
`Shuttle setup instructions <../../Shuttle/README.rst#a-first-executable>`_
explain how to build both tools.

1. Run Hello World
------------------

From the directory where you keep your projects, create a project named
``hello`` and run it.

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

Execution starts in ``main``. The ``using`` line imports ``println``, which
writes the greeting to the terminal.

2. Greet someone by name
------------------------

Edit the same ``hello/src/main.co`` file.

#. Add ``input`` to the import. At the start of ``main``, prompt for a name
   and store the reply in a ``string`` named ``name``. A blank line gives
   an empty string. If input ends before a name is entered, ``input``
   returns ``null``, and ``?: ""`` supplies the empty string.
#. Change ``"Hello, world!"`` to ``#"Hello {name}!"``. The ``#`` starts an
   interpolated string, so ``{name}`` inserts the name you typed.
#. Give ``main`` the return type ``isize`` so it can return an exit status.
   Before printing, return ``-1`` if ``name.isEmpty()`` is true. After
   printing, return ``0`` to indicate success.

The finished file should look like this.

.. code-block:: text

   using cloth.Console : println, input;

   pub fn<isize> main() {
       string name = input("What is your name? ") ?: "";
       if (name.isEmpty()) return -1;

       println(#"Hello {name}!");

       return 0;
   }

From the same directory where you ran ``shuttle init hello``, run the
project again.

.. code-block:: sh

   shuttle run hello

Type ``Ada`` at the prompt and press Enter.

.. code-block:: text

   What is your name? Ada
   Hello Ada!

Run it once more and press Enter without typing a name to try the early
return.
