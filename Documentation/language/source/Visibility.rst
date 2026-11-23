.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Names and visibility
====================

A Cloth identifier starts with an ``XID_Start`` character or ``_``, followed
by zero or more ``XID_Continue`` characters. A lone ``_`` is the discard or
placeholder token. Cloth uses the Unicode 18.0.0 UAX #31 profile R1-2 and
compares identifiers case-sensitively in NFC (R4), with no normalization
exclusions. For example, both Unicode spellings of ``café`` resolve to the
same declaration. Diagnostics retain the original source spelling.
See `Unicode Standard Annex #31 <https://www.unicode.org/reports/tr31/tr31-45.html>`_.

An unmarked top-level declaration is private to its file. An unmarked
member is private to its declaring type, even when the type is public.
``pub`` is the only visibility modifier.

A ``using`` declaration imports public names from another file. The dotted
module path selects a ``.co`` or ``.clib`` file. The compiler searches the
entry file's directory first, then the configured standard library.
Inside a function or method, parameters and local bindings take precedence
over members and file-level names.

Importing a module
------------------

Put imports before declarations. Select one or more names after the
colon, or use ``*`` to bring in all the file's public declarations.

.. code-block:: text

   using cloth.io.path.Path: Path, pathOf
   using cloth.math.Math: *

   fn<Path> manifest() = pathOf("module.toml")
   fn<float<64>> circleConstant() = TAU()

From the repository root, pass ``--stdlib Stdlib`` to check this example.
The module ``cloth.io.path.Path`` maps to ``cloth/io/path/Path.co`` under
that root. If both ``.co`` and ``.clib`` match the same module in the selected
directory, the import is ambiguous. Listing a name twice in one import
reports ``C2010``. An importing file's own imports do not re-export those names.

Member visibility
-----------------

Use a dotted name such as ``Outer.Inner`` for a visible nested type. A
public declaration must also expose types that its callers can name.
A public function with a private result type reports ``C3117``.

A positional struct literal initializes fields in declaration order. Each
field must be accessible where the literal appears. A struct's own method
can initialize its private fields. A free function in the same file can
use a positional literal only if all the fields are public.

Mark a state declaration or attachment ``pub`` so callers outside the class
can name that state in a test or transition. For initial states, payloads,
and the visibility of a separate state definition, see `Named states
<../objects/States.rst#declaring-and-attaching-states>`_.

A class method that implements a public interface requirement must itself
be declared ``pub``. If the interface declares the method without a body,
the class can supply the first body with either ``pub mt`` or
``pub override mt``. A class method that replaces an inherited interface
body introduced by ``= default`` must use ``override``.
A derived class can satisfy an interface it adds with a matching public
method inherited from its base.
