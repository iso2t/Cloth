# Musubi

The standalone C core comes from [starwing/musubi at b5a6470](https://github.com/starwing/musubi/tree/b5a64709a79ef2431c7f05644c9c0b209e97af66). Its MIT license is in [LICENSE](LICENSE). The upstream generator derives `unidata.h` from the Unicode Character Database. [Upstream generator](https://github.com/starwing/musubi/blob/b5a64709a79ef2431c7f05644c9c0b209e97af66/misc/parseucd.lua) · [Unicode license](UNICODE-LICENSE)

The vendored header fixes the Windows default charset symbol and includes exact line starts in its byte-to-line search. It also guards the multiline-label sort because groups with only single-line labels have a null multiline array. The build compiles `src/musubi_impl.c` as C and defines `MU_API` to support static linking on Windows.
