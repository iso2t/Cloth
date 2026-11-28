# Cloth Standard Library

The standard library's [build.k](build.k) declares the [StandardLibrary module](StandardLibrary/module.k).
Its Cloth source root is `StandardLibrary/src/`, with a `cloth/` directory for public import paths.

The library entry is `cloth/Library.co` relative to that root. It imports
collection and numeric declarations for the compiler's smoke check. A consumer
imports the public names it uses from their defining modules.

The public generic [List](StandardLibrary/src/cloth/collections/README.md) is implemented in
`cloth/collections/List.co`, with independent value storage and explicit lifetime.
It supports direct foreach through `Snapshot<T>` in `cloth/collections/Snapshot.co`.
[`OwningList<T>`](StandardLibrary/src/cloth/collections/README.md#owninglist) takes deletion authority
over class objects and supplies checked reads, snapshot foreach, and ordered cleanup.

Import paths start at the standard library source root, the directory containing
`cloth/`. For example, `using cloth.collections.List : List` selects the public
declaration from `cloth/collections/List.co`. A declaration must be `pub` for
another module to import it.

Import the console functions a source file uses, for example
`using cloth.Console : println;`. The standard library defines `print`,
`println`, and `input` in [Console.co](StandardLibrary/src/cloth/Console.co).

## Validate the library sources

Run these commands from this directory (`Stdlib/`) in the Cloth checkout.
Use `mingw32-make` in place of `make` on Windows with MinGW.

```sh
make -C .. run
make -C .. test-file CASE=Tests/Compiler/Semantic/stdlib-modules.test
make -C .. test-file CASE=Tests/Compiler/Driver/stdlib-native.test
```

The first command checks `cloth/Library.co` and its imports with the native
frontend. The second checks the entry and imports every other shipped module
from a separate consumer at both 32 and 64 bits. The third links and runs
consumers of the collection, math, text, path, and file modules. See the
[full-library fixture](../Tests/Compiler/Semantic/stdlib-modules.test) and
[test prerequisites](../Documentation/contributing/Testing.rst#test-dependencies).

To run the collection example from the repository root, compile the consumer
and link its object with the runtime archive for the target.

```sh
build/cloth --kind executable --src Tests/Compiler/Driver/Inputs/stdlib-collections.co --emit-object build/collections.o
cc build/collections.o "build/lib/cloth/$(build/cloth --print-target)/libcloth_runtime.a" -o build/collections
```

Running `build/collections` prints `40` and `2` on separate lines as the
owned children are destroyed, then exits with status 42. On Windows use
`build/cloth.exe`, MinGW `gcc`, and `build/collections.exe`, adding
`-lshell32` to the link command. For the
additional text and file link inputs, see the [runtime dependencies](../Documentation/compiler/driver/Objects.rst#runtime-dependencies).

### Check a single module

To check edits to `cloth.text.Utf8`, save this one-line consumer as
`build/check.co` in the repository checkout.

```cloth
using cloth.text.Utf8: *;
```

From the repository root, run the checks against the working copy of the
standard library. On Windows, use `build/cloth.exe`.

```sh
build/cloth --kind library --stdlib Stdlib/StandardLibrary/src --target-bits 32 --src build/check.co
build/cloth --kind library --stdlib Stdlib/StandardLibrary/src --target-bits 64 --src build/check.co
```

A successful check prints nothing and exits with status `0`. Passing `Utf8.co`
directly to `--src` reports `C3136` because its `text.utf8.decode` intrinsic
requires an import from the standard library selected by `--stdlib`. To check
`File.co` or `OwningList.co`, change the import path in `check.co` to
`cloth.io.fs.File` or `cloth.collections.OwningList`, respectively.

## Strings, characters, and booleans

`String`, `Char`, and `Bool` supply methods on the primitive types
`string`, `char`, and `bool`. With the standard library selected, a call
such as `text.trim()` needs no import. String indices count Unicode scalar
values, and `split` accepts either a character or a string separator.
See the [primitive declarations](StandardLibrary/src/cloth/lang/Primitive.co)
and [method reference](../Documentation/language/expressions/Builtins.rst#character-and-string-methods).

## Numbers and math

The standard library prelude makes `Number` available in every source file.
Use it in a bound such as `fn<T> twice<T: Number>(T value) = value + value;`. Signed and unsigned
integers, target-sized integers, and floating-point types satisfy this
interface. A numeric type parameter retains its caller's exact type. Classes
can implement `Number` explicitly and provide the operators their callers use.
See the [prelude](StandardLibrary/src/cloth/lang/Prelude.co).

The library declares `abs`, `clamp`, `signum`, and `::str` on both
`Integer` and `Float`. Integers also provide `smaller`, `leadingZeros`,
`trailingZeros`, and `toStringRadix`. Within each family, `Self` preserves
the receiver's exact type. See the [Cloth declarations](StandardLibrary/src/cloth/lang/num/Number.co)
and [numeric method reference](../Documentation/language/expressions/Builtins.rst#standard-library-numeric-methods).

The `cloth.math.Math` module exports `E()`, `PI()`, and `TAU()` as
64-bit floating-point values. The `magic<T>(T value)` function in
`cloth.lang.num.NumberUtilities` returns its argument with the same type.
See the [math functions](StandardLibrary/src/cloth/math/Math.co) and
[numeric utility](StandardLibrary/src/cloth/lang/num/NumberUtilities.co).

## Paths, files, and text

`pathOf(string spelling)` constructs a `Path` whose public `spelling`
field holds the supplied string. `File::new(path)` accepts a `Path` or a
`string`. Import the path and file types separately.

```cloth
using cloth.io.path.Path: Path, pathOf;
using cloth.io.fs.File: File, FileError;
using cloth.text.Utf8: decodeUtf8;

fn<Path> manifest() = pathOf("module.k");
fn<File> reader(Path path) = File::new(path);
fn<FileError?> openFile(File file) = file.open();
fn<unsigned int<8>[]?, FileError?> chunk(File file, usize limit) = file.read(limit);
fn<string?> asText(unsigned int<8>[] bytes) = decodeUtf8(bytes);
```

`file.open()` returns a nullable `FileError`. `file.read(maxBytes)`
returns two ordered results, a nullable byte array and a nullable error.
The declared cases are `InvalidPath`, `NotFound`, `PermissionDenied`,
`OpenFailed`, `AlreadyOpen`, `NotOpen`, and `ReadFailed`. UTF-8
decoding takes a byte array and returns an independent string on success
or `null` on malformed input. A successful read at end of file returns a
present empty byte array and a null error. Deleting the file closes the
native stream. See the [file declarations](StandardLibrary/src/cloth/io/fs/File.co),
[path type](StandardLibrary/src/cloth/io/path/Path.co), and [UTF-8 declaration](StandardLibrary/src/cloth/text/Utf8.co).

## Use library sources

The compiler finds the bundled sources in `lib/cloth/stdlib` relative to
its executable. To use a working copy, pass `--stdlib PATH` with the directory
containing `cloth/`. The frontend checks reachable modules together.
For collection APIs, see
the [collections guide](StandardLibrary/src/cloth/collections/README.md).
