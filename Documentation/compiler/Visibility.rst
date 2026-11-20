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

A positional struct literal initializes fields in declaration order. Each
field must be accessible where the literal appears. A struct's own method
can initialize its private fields. A free function in the same file can
use a positional literal only if all the fields are public.

A class method that implements a public interface requirement must itself
be declared ``pub``. If the interface declares the method without a body,
the class can supply the first body with either ``pub mt`` or
``pub override mt``. A class method that replaces an inherited interface
body introduced by ``= default`` must use ``override``.
A derived class can satisfy an interface it adds with a matching public
method inherited from its base.
