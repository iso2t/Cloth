.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Names and visibility
====================

An unmarked top-level declaration is private to its file. An unmarked
member is private to its declaring type, even when the type is public.
``pub`` is the only visibility modifier. Both ``private`` and ``priv`` are
ordinary identifiers.

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
be declared ``pub``, including when it uses ``override:``. A derived class
can satisfy an interface it adds with a matching public method inherited
from its base.
