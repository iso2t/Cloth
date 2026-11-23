.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Writing Cloth documentation
===========================

Choose a page by the question a reader wants to answer. When a change
affects what a programmer can write or how someone uses the compiler,
update the relevant guide in the same pull request.

Choose a page
-------------

* **Write a Cloth program.** A first-program walkthrough shows the source,
  how to run it, and the expected result. Explain language rules in the
  language reference, as in `Calls and results <../compiler/Calls.rst>`_.
* **Use the compiler.** Show the command a task needs in its walkthrough.
  Keep the full description of an option such as ``--emit-object`` in the
  `compiler flags reference <../compiler/Flags.rst>`_.
* **Contribute to Cloth.** Put workflows such as testing and submitting
  changes with the `contributor guides <../Welcome.rst#contributing>`_.
  Explain how a subsystem works in its component overview, as in the
  `compiler README <../../Compiler/README.rst>`_.

Review the layout
-----------------

Pick a reader's question, such as how Cloth chooses an overload. Try to
reach the answer by following links from the `repository README
<../../README.rst>`_ or the `welcome page <../Welcome.rst>`_.

If the answer is hard to find, add a link whose text names the topic.
Keep the full explanation on one page so future changes have one place
to update. When moving a page or renaming a heading, update links to
its old path or section.

Check an example
----------------

Check the behavior in the implementation and choose a test that exercises
the claim. A frontend check can show that the compiler accepts a program.
For a claim about output, use a test that runs the program.

Link to the tested source so readers can run the same example. The
``println`` test checks that the program prints ``1`` on a line and exits
with status ``42``. See the
`example program <../../Tests/Compiler/Driver/Inputs/println.co>`_ and
its `driver test <../../Tests/Compiler/Driver/println.test>`_.

Run this case from the repository root. On Windows with MinGW, use
``mingw32-make`` in place of ``make``.

.. code-block:: console

   make test-file CASE=Tests/Compiler/Driver/println.test
