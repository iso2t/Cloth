.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Cloth compiler manual
=====================

Building and compiling
----------------------

* `Build the compiler <../building/Building.rst>`_
* `Run your first Cloth program <../getting-started/FirstProgram.rst>`_
* `Command-line options <Flags.rst>`_
* `Emit and link an object file <driver/Objects.rst>`_
* `Read a compiler diagnostic <driver/Diagnostics.rst>`_

Target layout
-------------

* `Integer size, alignment, and array stride <storage/Integers.rst>`_
* `Structs, enum tags, and nullable storage <storage/Aggregates.rst>`_

.. _diagnostics-and-internals:

For compiler contributors
-------------------------

* `From source to a checked program <internals/Frontend.rst>`_
* `Lowering a checked program to a native object <internals/Backend.rst>`_
* `Maintaining diagnostic codes and source labels <../../Diagnostics/README.rst>`_
