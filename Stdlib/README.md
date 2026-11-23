# Cloth Standard Library

The standard library is a Cloth project. All library source files live under
`cloth/`, with ordinary subfolders for each area of the library.

The library entry, `cloth/Library.co`, imports collection and numeric
declarations for the compiler's smoke check. A consumer imports the public
names it uses from their defining modules.

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

## Numbers and math

Import `using cloth.lang.num.Number : Number` to write a numeric bound such
as `fn<T> twice<T: Number>(T value) = value + value`. Signed and unsigned
integers, target-sized integers, and floating-point types satisfy this
interface. A numeric type parameter retains its caller's exact type. Classes
can implement `Number` explicitly and provide the operators their callers use.

The `cloth.math.Math` module exports `E()`, `PI()`, and `TAU()` as
64-bit floating-point values. The `magic<T>(T value)` function in
`cloth.lang.num.NumberUtilities` returns its argument with the same type.
See the [math functions](cloth/math/Math.co) and
[numeric utility](cloth/lang/num/NumberUtilities.co).

## Paths, files, and text

`pathOf(string spelling)` constructs a `Path` whose public `spelling`
field holds the supplied string. `File::new(path)` accepts a `Path` or a
`string`. Import the path and file types separately.

```cloth
using cloth.io.path.Path: Path, pathOf
using cloth.io.fs.File: File, FileError
using cloth.text.Utf8: decodeUtf8

fn<Path> manifest() = pathOf("module.toml")
fn<File> reader(Path path) = File::new(path)
fn<FileError?> openFile(File file) = file.open()
fn<unsigned int<8>[]?, FileError?> chunk(File file, usize limit) = file.read(limit)
fn<string?> asText(unsigned int<8>[] bytes) = decodeUtf8(bytes)
```

`file.open()` returns a nullable `FileError`. `file.read(maxBytes)`
returns two ordered results, a nullable byte array and a nullable error.
The declared cases are `InvalidPath`, `NotFound`, `PermissionDenied`,
`OpenFailed`, `AlreadyOpen`, `NotOpen`, and `ReadFailed`. UTF-8
decoding takes a byte array and returns `string?`. File access and UTF-8
decoding are supported for source checking only. See the [file declarations](cloth/io/fs/File.co),
[path type](cloth/io/path/Path.co), and [UTF-8 declaration](cloth/text/Utf8.co).

## Use library sources

Pass `--stdlib PATH` to the native compiler with the directory containing
`cloth/`. The frontend resolves the consumer's `using` imports against that
source root and checks reachable modules together. For collection APIs, see
the [collections guide](cloth/collections/README.md).
