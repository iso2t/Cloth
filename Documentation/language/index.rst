.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Cloth language reference
========================

Check a source file with ``cloth --src file.co``. For the types and operations
you can compile and link into an executable, see `Native object support
<../compiler/driver/Objects.rst#source-checks-and-native-code>`_.

.. _names-and-functions:

Source files and functions
--------------------------

* `Source files, comments, and literals <source/Source.rst>`_
* `Names, imports, and visibility <source/Visibility.rst>`_
* `Function calls, arguments, and return values <expressions/Calls.rst>`_

Types
-----

* `Primitive and nullable types <types/Types.rst>`_
* `Array types, indexing, and copying <types/Arrays.rst>`_
* `Struct, enum, class, and interface declarations <types/Declarations.rst>`_
* `Generic types, calls, and bounds <types/Generics.rst>`_

Objects
-------

* `Object ownership and deletion <objects/Ownership.rst>`_
* `Named states and transitions <objects/States.rst>`_

Expressions and statements
--------------------------

* `Local bindings and assignment <statements/Bindings.rst>`_
* `Operators, arithmetic, conversions, and equality <expressions/Operators.rst>`_
* `Member access, data providers, and type information <expressions/Access.rst>`_
* `Built-in functions and methods <expressions/Builtins.rst>`_
* `Control flow <statements/ControlFlow.rst>`_ covers conditionals, loops,
  ``break``, and ``continue``. For ``operator foreach()``, see
  `Collection iteration <statements/ControlFlow.rst#collection-iteration>`_.
