.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Welcome!
========

Hello Weavers! This is the main documentation for the Cloth compiler project.

The Cloth compiler is the core of the Cloth programming language. Cloth is defined
as a modern, strongly-typed object-oriented language aimed at low level systems work
with an upfront a high level type system. It feels somewhat familiar to C-style
languages with inspirations taken from many languages, including C, C++, Java, Kotlin,
and Go.

Cloth is not a garbage compiled language. Instead, it has an owner/operator model, where
only the owner of the reference can destroy the reference. The main goal for Cloth is to
hide boiler plate pointers and references.

Cloth also incorporates a first class state system, in which an object can exist in multiple
states or no states, allowing certain parameters to be valid or invalid, moving beyond just
normal boolean expressions with explicit syntax.

`Operator overloads and equality <compiler/Operators.rst>`_ describes
how user-defined types participate in expressions and updates.

Contributing
------------

Cloth welcomes contributions to the compiler and its documentation.
Submit changes for review through a pull request. The terms for
submitting contributions are set out in the project's
`Apache License 2.0 with the LLVM exception <../LICENSE>`_.

* For AI-assisted changes, follow the review, verification, and
  disclosure requirements in `AI Agents <contributing/AI%20Agents.rst>`_.
* Propose significant changes to the language or compiler through the
  `RFC Process <contributing/RFC%20Process.rst>`_.
* Follow the C++ naming, formatting, and license header conventions in
  `Code Style <contributing/Code%20Style.rst>`_.
* Run the regression suite or a focused case with the commands in
  `Testing <contributing/Testing.rst>`_.
