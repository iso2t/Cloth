.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Local bindings and assignment
=============================

Write a type before a local name, or use ``def`` to infer it from the
initializer. Use ``=`` for a mutable binding and ``:=`` for an immutable
one. The following program returns 42 because ``saved`` keeps the value
it had when it was bound. See the `local assignment examples
<../../../Tests/Compiler/Driver/mutable-locals.test>`_.

.. code-block:: text

   pub fn<int> main() {
       int source = 42
       def saved := source
       source = 7
       return saved
   }

A mutable numeric local also accepts ``+=``, ``-=``, ``*=``, ``/=``,
``%=``, and standalone ``++`` or ``--``. To start a nullable integer
at ``null``, write ``int? count = null``. A ``for`` loop also accepts
a type or ``def`` before its binding name. See `Control flow
<ControlFlow.rst>`_.

Receiving several results
-------------------------

A function's results are ordered. Give each result a typed name, or
use ``def`` to infer the types.

.. code-block:: text

   fn<int, int> limits() = 1, 4

   fn<int> span() {
       int low, int high = limits()
       return high - low
   }

In an assignment, ``_, high = limits()`` discards the first result and
stores the second in ``high``. A single value can initialize several
names. In ``int low, high = 0``, both bindings receive zero. See
`Calls and results <../expressions/Calls.rst#return-values-and-bindings>`_.

Deferred bindings
-----------------

A block initializer can defer a computation until the name is read.
Give its results explicit types. Each binding is read-only, even with
``=``. In ``int n, bool ok = { return 1, true }``, reading ``n`` runs
the block and selects the first result. Reading ``ok`` runs it again
and selects the second. Any writes to captured variables take effect
on each evaluation.
