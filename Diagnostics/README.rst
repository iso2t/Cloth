Cloth Diagnostics
=================

Diagnostics is the reporting subsystem for the Cloth compiler. The lexer and
parser use it to report problems such as malformed numeric literals,
unexpected tokens, and misplaced imports.

Each diagnostic has a stable code, a severity, and a message that describes
what happened. When source text is available, the report can highlight the
relevant text and point to related locations. It can also suggest a fix.
For a report seen at the command line, see `Reading compiler diagnostics
<../Documentation/compiler/driver/Diagnostics.rst>`_.

Codes and source labels
-----------------------

``DiagnosticCode`` assigns numeric values explicitly. Its public spelling
is ``C`` followed by four digits, for example ``C2010``. Preserve an existing
code's meaning and leave retired numbers reserved when changing the
`code definitions <include/cde/diagnostic_code.hpp>`_. A diagnostic
carries its severity, message, ordered source labels and optional help.
Use an existing error factory for a repeated error, such as the
`duplicate import selection <src/cde/import_errors.cpp>`_.

Store labels against the original ``SourceFile``. Lines and byte columns
start at one and the end of the span is exclusive. An identifier's
normalized spelling can differ in byte length from the source token.
Highlight the token's original span. Put the location the reader should
act on first, then related sites such as a declaration or earlier use.

The `reporter <src/cde/diagnostic_reporter.cpp>`_ gives Musubi a separate
source for each file and selects byte indexing. The first valid label
becomes primary. If rendering fails, the reporter still writes the code,
severity, message, and any help. The `import diagnostic unit test
<../Tests/Diagnostics/Unit/import_test.cpp>`_ checks both label spans,
and the `command-line fixture <../Tests/Diagnostics/repeated-import.test>`_
checks the displayed code and labels.
