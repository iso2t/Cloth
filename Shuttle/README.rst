Shuttle
=======

Shuttle is the Rust CLI for the Cloth build system. The ``shuttle build``
command reports that object emission is unavailable through Shuttle
and exits with a failure status.

To validate the standard library, run ``make run`` from the Cloth
repository root, or ``mingw32-make run`` on Windows with MinGW. This
checks the library entry and its imports with the native frontend.
The `Stdlib guide <../Stdlib/README.md#validate-the-library-sources>`_
also covers validation of every shipped module at both target widths.
