# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from pathlib import Path
import sys


WIDTHS = (1, 7, 8, 9, 12, 15, 16, 17, 24, 31, 32, 33, 63, 64, 65, 97,
          127, 128, 129, 257, 1025, 4097, 65535, 65536)


def main():
    checks = []
    functions = []
    for width in WIDTHS:
        for prefix, limit in (("", "min"), ("unsigned ", "max")):
            element = f"{prefix}int<{width}>"
            name = f"{'unsigned' if prefix else 'signed'}{width}"
            marker = f"{element}::{limit}"
            update, wrapped = ("++", "0") if prefix else ("--", f"{element}::max")
            checks.append(f"    if (!{name}(1)) {{ println({width if prefix else -width}); return 1; }}")
            functions.append(f"""fn<bool> {name}(final usize index) {{
    {element}[3] values = [{marker}, 0, {marker}];
    def saved := values;
    if (!(values == saved)) return false;
    values[index] = {marker};
    if (values[1] != {marker} || values == saved) return false;
    values[index] = 0;
    if (values[1] != 0 || saved != values) return false;
    values[index] += values[0];
    if (values[1] != {marker}) return false;
    values[index]{update};
    return values[0] == {marker} && values[1] == {wrapped} && values[2] == {marker};
}}""")
    entry = "\n".join(["pub fn<int> main() {", *checks, "    return 42;", "}"])
    Path(sys.argv[1]).write_text("\n\n".join(["using cloth.Console : println;", entry, *functions]) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
