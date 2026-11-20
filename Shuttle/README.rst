Shuttle
=======

Shuttle is the Rust CLI for the Cloth build system. Object emission is
unavailable in this checkout. The ``shuttle build`` command reports this
on stderr and exits with a failure status.

To validate the standard library, run ``make run`` from the Cloth
repository root, or ``mingw32-make run`` on Windows with MinGW. This
checks the library entry and its imports with the native frontend.
The `Stdlib guide <../Stdlib/README.md#validate-the-library-sources>`_
also covers validation of every shipped module at both target widths.
