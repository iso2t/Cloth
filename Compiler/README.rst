Cloth Compiler
==============

The compiler's front end starts with **lexing**, which turns Cloth source text
into tokens such as keywords, identifiers, and operators. **Parsing** builds a
syntax tree from those tokens to represent the structure of the program.

**Semantic analysis** checks that the parsed program follows the rules of
Cloth. This includes resolving names and checking that types are used
consistently.

The compiler translates the checked program into LLVM intermediate
representation (IR). LLVM then lowers that IR to assembly for the target
architecture.
