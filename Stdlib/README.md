# Cloth Standard Library

The standard library is a Cloth project. All library source files live under
`cloth/`, with ordinary subfolders for each area of the library.

```text
build.toml
module.toml
cloth/
    Library.co
    collections/
        List.co
        OwningList.co
        Snapshot.co
```

The public generic [List](cloth/collections/README.md) is implemented in
`cloth/collections/List.co`, with independent value storage and explicit lifetime.
It supports direct foreach through `Snapshot<T>` in `cloth/collections/Snapshot.co`.
[`OwningList<T>`](cloth/collections/README.md#owninglist) takes deletion authority
over class objects and supplies checked reads, snapshot foreach, and ordered cleanup.

Import paths start at the standard library source root, the directory containing
`cloth/`. For example, `using cloth.collections.List : List` selects the public
declaration from `cloth/collections/List.co`. A declaration must be `pub` for
another module to import it.

## Validate the library sources

Run these commands from this directory (`Stdlib/`) in the Cloth checkout.
Use `mingw32-make` in place of `make` on Windows with MinGW.

```sh
make -C .. run
make -C .. test-file CASE=Tests/Compiler/Semantic/stdlib-modules.test
```

The first command checks `cloth/Library.co` and its imports with the native
frontend. The second checks the entry and imports every other shipped module
from a separate consumer at both 32 and 64 bits. See the
[full-library fixture](../Tests/Compiler/Semantic/stdlib-modules.test) and
[test prerequisites](../Documentation/contributing/Testing.rst#test-dependencies).

## Use library sources

Import `using cloth.lang.num.Number : Number` to write a numeric bound such
as `fn<T> twice<T: Number>(T value) = value + value`. Signed and unsigned
integers, target-sized integers, and floating-point types satisfy this
interface. A numeric type parameter retains its caller's exact type. Classes
can implement `Number` explicitly and provide the operators their callers use.

Pass `--stdlib PATH` to the native compiler with the directory containing
`cloth/`. The frontend resolves the consumer's `using` imports against that
source root and checks reachable modules together. For collection APIs, see
the [collections guide](cloth/collections/README.md).
