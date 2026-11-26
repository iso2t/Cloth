.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Named states
============

A class instance can change which method a call selects by entering a
different state. The meter below starts in ``Idle``. Its ``Loaded`` state
carries an integer and supplies the ``reading()`` method.

.. code-block:: text

   pub typedef<class> Meter {
       pub state<int> ::reading();
       pub default state<void> Idle {}
       pub state Loaded;
   }

   typedef<state<int>> pub Loaded for Meter {
       mt<int> reading() = state;
   }

   fn<int> load(Meter meter) {
       def alias = meter;
       alias -> Loaded(42);
       return meter::reading();
   }

In ``load``, ``alias`` and ``meter`` refer to the same instance. Entering
``Loaded`` through ``alias`` selects the body of ``reading()`` for the call
through ``meter``. Inside that body, ``state`` is the integer supplied on
entry. See the `meter example <../../../Tests/Compiler/Semantic/Inputs/dispatch/meter.co>`_.

Declaring and attaching states
------------------------------

Use ``state<Payload> Name { ... }`` to define a state inside a class.
For an empty definition, write ``state<void> Ready;``. Use ``state Ready;``
to attach a separate definition with that name. A separate state
names its host with ``for``, as in ``typedef<state<void>> Ready for Lamp;``.
A state declared for an interface can be
attached to classes that implement it. To export a separate state from
its file, put ``pub`` after the ``typedef<state<...>>`` header or place
the definition in a file-level `public block <../source/Visibility.rst#public-declaration-blocks>`_.
The class controls the visibility of its own ``state Name;`` attachment.

A class that declares or attaches states must mark one of them
``default``. A derived class that only inherits states keeps its base
class's choice. A default state with a payload needs an initial value.
To start the meter with a reading of 1, move ``default`` from ``Idle``
to the attachment and write ``pub default(1) state Loaded;``.

Transitions and payloads
------------------------

Test the active state with ``meter is Loaded``. Use ``meter -> Loaded(42)``
to enter a state that takes an integer, or ``meter -> Idle`` to enter a
``void`` state. A transition changes the instance and has no result.
A state body can use ``self`` to access the host and ``init { ... }`` to
run code on entry. A running state method keeps the payload it received on
entry, even if it changes the object's state before returning. In the
`native state example <../../../Tests/Compiler/Driver/Inputs/state-dispatch.co>`_,
``finish()`` enters ``Ready`` and then reads both elements of the previous
state's array.

The payload must match the declared type. Changing the transition in
``load`` to ``alias -> Loaded(false)`` produces ``C3129`` because
``Loaded`` requires an ``int``. See the `rejected transition
<../../../Tests/Compiler/Semantic/Inputs/dispatch/wrong_payload.co>`_.

State methods
-------------

The host declaration ``pub state<int> ::reading();`` gives the method's
name and return type. A state provides its body with ``mt<int> reading()``.
A call written ``meter::reading()`` selects the active state's body.
Ordinary class methods use the dot in ``meter.method()``.

A state may omit a method. Calling ``meter::reading()`` immediately
after ``meter -> Idle`` produces ``C3130`` because ``Idle`` has no body
for ``reading()``. If the active state is only known at runtime, the same
call ends the program with status 1 and reports the missing method.
See the `missing state method example
<../../../Tests/Compiler/Semantic/invalid-active-state.co>`_.
