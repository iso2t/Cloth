.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Type declarations
=================

A ``typedef`` declaration names a struct, enum, class, or interface.
Structs group fields into a value. Enums name a set of cases. Classes
and interfaces declare methods on objects. An empty class, interface, or
struct can end directly after its header. For example,
``typedef<class> Marker;`` defines the same class as ``typedef<class> Marker {}``.

Structs
-------

A positional literal such as ``{true, {true, 42}}`` initializes fields
in declaration order. Each initializer is evaluated once. If one returns
from the function, later fields are skipped. Copying a struct copies
its nested struct fields as well. Separate consecutive fields with commas
and end the last field with a semicolon, including before the closing brace.

.. code-block:: text

   typedef<struct> Inner { pub bool ready, pub int<12> value; }
   typedef<struct> Sample { pub bool tagged, pub Inner inner; }

   pub fn<int> main() {
       Sample sample = {true, {true, 42}};
       def original := sample;
       sample.inner.value = 7;
       return if (sample.tagged && sample.inner.ready && original.inner.value == 42) sample.inner.value -> int else 1;
   }

This program returns 7. The write changes ``sample.inner.value``, and
the earlier copy still holds 42. Numeric fields of a mutable local also
support compound assignment and standalone increment or decrement. See the
`nested struct example <../../../Tests/Compiler/Driver/Inputs/nested-struct.co>`_.

A field can itself have a nullable type. A nullable struct containing
all zero fields is present. After checking for ``null``, fields can be
read and written through the stored value. See the `nullable struct
example <../../../Tests/Compiler/Driver/Inputs/nullable-struct-values.co>`_.

Copying a range field preserves both endpoints and whether the end is
included. In the `range field example
<../../../Tests/Compiler/Driver/Inputs/struct-range-fields.co>`_,
``window.bounds`` holds ``13..=15`` when the program copies the struct.
After ``window.bounds = null``, the check ``15 : saved.bounds`` still succeeds.

A struct's storage must be finite. A field cannot contain the same struct
again by value, even through another struct or a nullable field. A class
reference can link a struct back to an object that holds it. See the
`recursive field examples
<../../../Tests/Compiler/Semantic/Unit/generic_instances_test.cpp>`_.

Enums
-----

An enum requires at least one case. Separate its cases with commas and
end the list with one semicolon. Cases have integer tags starting at zero
in declaration order, including private cases. A case can also supply
values for named, read-only payload fields. A field read selects the
value for that case, using the declared
default when the case omits it.

.. code-block:: text

   typedef<enum> Mode(pub def<int> code = 7) { pub Off, pub On(42); }

   pub fn<int> main() {
       Mode mode = Mode.On;
       return mode.code;
   }

``Mode.Off.code`` is 7. The program selects ``Mode.On`` and returns 42.
A nullable payload field can default to ``null``. Built-in ``==`` and
``!=`` compare tags of the same enum, so two cases with identical
payloads can compare unequal. Casting a case or stored enum to ``int``
gives its tag. Casting a nullable enum to ``int?`` preserves absence.
See the `enum example <../../../Tests/Compiler/Driver/Inputs/enum-payload.co>`_
and its `payload reads <../../../Tests/Compiler/Driver/Inputs/enum-payload-reads.co>`_.

Classes and interfaces
----------------------

An interface can require a method or supply a default body. A method
signature and a parenthesized default each end with a semicolon. A
braced default ends at its closing brace. A class
lists its base class and implemented interfaces after its name and
constructor parameters. A class can inherit the interface default or
replace it with a public override. A class has at most one base class.
Write ``final typedef<class>`` to prevent other classes from inheriting it.
A class can declare a destructor as ``~Name { ... }``. See `Object ownership
<../objects/Ownership.rst>`_ for what happens to aliases when it runs.

.. code-block:: text

   typedef<interface> Reading {
       pub mt<int> read() = default (7);
   }

   typedef<class> Gauge : Reading {
       pub override mt<int> read() = 42;
   }

   fn<int> measure(Reading item) = item.read();

When ``measure`` takes a ``Gauge``, ``item.read()`` selects the override
in ``Gauge``. A class can use ``pub virtual mt`` to let derived classes
override a method in the same way. See the `dispatch tests
<../../../Tests/Compiler/Semantic/Unit/dispatch_test.cpp>`_.

The interface used at the call site supplies argument labels and omitted
defaults. The instance supplies the selected method body. Two inheritance
paths to the same default can share it. If two independent interfaces
supply competing defaults for the same method, the class must choose a
body with ``pub override mt``.

For creating an instance and initializing its fields, see `Constructors
<../expressions/Calls.rst#constructors>`_. For access to fields, cases,
and methods, see `Names and visibility <../source/Visibility.rst>`_.
A class can also declare a `state method <../objects/States.rst#state-methods>`_
whose implementation depends on the instance's active state.
