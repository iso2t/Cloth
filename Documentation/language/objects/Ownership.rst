.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Object ownership
================

Use ``Type::new(...)`` to construct a class instance. Copying its
reference into another binding makes the two names aliases. Deleting the
instance through either alias ends its lifetime for both. For constructor
arguments and visibility, see `Constructors <../expressions/Calls.rst#constructors>`_.

Aliases and deletion
--------------------

In the following function, the compiler rejects the read through ``alias``.

.. code-block:: text

   pub typedef<class> Item (pub def<int> value);
   fn<int> read(Item item) {
       def alias = item;
       item::delete;
       return alias.value;
   }

The diagnostic ``C3092`` points to ``alias.value`` and the earlier
``item::delete``. To keep the integer, read ``alias.value`` into a
local before deleting the instance, then return that local. See the
`deleted alias example <../../../Tests/Compiler/Semantic/deleted-alias.co>`_.

Deleting a null class reference has no effect. Deleting the same instance
twice, even through different names, is an error.

Deleting through a base class or interface reference runs the concrete
class's destructor first, followed by each base class destructor. Each
destructor runs once. It can read its own fields through ``self``.
An outside alias cannot access the instance while destruction is underway.

When an argument can delete a method's receiver, the call checks the
captured receiver again after evaluating the arguments. For example,
``item.read(destroy(item))`` runs ``destroy`` and reports a runtime
error before entering ``read`` if the argument deleted ``item``.
Field assignments also check the receiver when the right-hand side can
invalidate it. A later allocation leaves every alias to the deleted
instance stale. See the `linked lifetime examples
<../../../Tests/Compiler/Driver/class-lifetime.test>`_.

Owning lists
------------

Import ``OwningList`` with ``using cloth.collections.OwningList: OwningList;``.
``OwningList<Item>::new`` creates an empty list. Appending an object gives
the list responsibility for deleting it. ``list.get(i)`` and ``list[i]``
return aliases. A write such as ``list[i].value = 10`` changes the object's
field.

If you try to delete an object the compiler knows belongs to a list, it
reports ``C3138``. If it can trace ownership to a specific ``append``, it
marks that call too. See the
`ownership diagnostic example <../../../Tests/Compiler/Semantic/ownership-flow.test>`_.

An object can occupy one slot in one owning list at a time.
``removeAt(i)`` releases the object and returns a live reference to it.
To transfer it, pass that reference to the other list's ``append``.
Owning lists can contain other lists. Two lists that own each other
would form an invalid ownership cycle.

An existing alias can also delete a released object. Here ``first``
already refers to entry zero. Removing that entry makes ``first::delete``
valid. Deleting ``list`` destroys the remaining entry. See the
`owning list example <../../../Tests/Compiler/Semantic/owning-list.co>`_.

.. code-block:: text

   list.removeAt(0);
   first::delete;
   list::delete;

Use ``list.clear()`` to delete the entries in list order and keep the
empty list available for reuse. See the `OwningList API
<../../../Stdlib/StandardLibrary/src/cloth/collections/README.md#owninglist>`_ for its element
types and operations.
