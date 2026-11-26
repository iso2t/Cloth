.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Welcome!
========

Hello Weavers! This is the main documentation for the Cloth compiler project.

The Cloth compiler is the core of the Cloth programming language. Cloth is a modern,
strongly-typed object-oriented language aimed at low level systems work
with an upfront a high level type system. It feels somewhat familiar to C-style
languages with inspirations taken from many languages, including C, C++, Java, Kotlin,
and Go.

Cloth gives programmers explicit control over object lifetimes. References to
an instance share its lifetime, and an owning list can take responsibility for
deleting it. See `Object ownership <language/objects/Ownership.rst>`_.

A class can define named states with their own payloads and methods. Each
instance of that class has one active state at a time. See `Named states
<language/objects/States.rst>`_.

Manuals
-------

* `Run your first Cloth program <getting-started/FirstProgram.rst>`_
* `Cloth language reference <language/index.rst>`_
* `Build projects and link libraries with Shuttle <../Shuttle/README.rst>`_
* `Cloth compiler manual <compiler/index.rst>`_
* `Standard library modules <../Stdlib/README.md>`_

Contributing
------------

Cloth welcomes contributions to the compiler and its documentation.
Submit changes for review through a pull request. The terms for
submitting contributions are set out in the project's
`Apache License 2.0 with the LLVM exception <../LICENSE>`_.

* Follow a source file through `frontend checking <compiler/internals/Frontend.rst>`_
  and `native code generation <compiler/internals/Backend.rst>`_.
* For AI-assisted changes, follow the review, verification, and
  disclosure requirements in `AI Agents <contributing/AI%20Agents.rst>`_.
* Propose significant changes to the language or compiler through the
  `RFC Process <contributing/RFC%20Process.rst>`_.
* Follow the C++ naming, formatting, and license header conventions in
  `Code Style <contributing/Code%20Style.rst>`_.
* For help placing a page or checking an example, see the guide to
  `writing Cloth documentation <contributing/Documentation.rst>`_.
* Run the regression suite or a focused case with the commands in
  `Testing <contributing/Testing.rst>`_.
