Cloth Diagnostics
=================

Diagnostics is the reporting subsystem for the Cloth compiler. The lexer and
parser use it to report problems such as malformed numeric literals,
unexpected tokens, and misplaced imports.

Each diagnostic has a stable code, a severity, and a message that describes
what happened. When source text is available, the report can highlight the
relevant text and point to related locations. It can also suggest a fix.
