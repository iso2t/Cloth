.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

AI Agents
=========

Contributors are welcome to use AI coding tools to help develop the
Cloth compiler. You are responsible for the entire contribution,
including any code, tests, build files, or documentation an agent
drafts or edits.

Using an agent
--------------

* Keep the agent's work focused on the problem you are solving. Follow
  Cloth's existing code style and build conventions.
* Share only material you have permission to send to the tool. Keep
  credentials and private data out of prompts and uploads. Check the
  source and license of any third-party material you incorporate and
  preserve required attribution.

Reviewing and verifying changes
-------------------------------

* Read the full diff yourself before submitting. Inspect deletions and
  changes to tests as well as additions. Keep only changes you understand
  and can explain to a reviewer.
* Check claims about Cloth syntax, compiler behavior, and APIs against
  the code and documentation. Confirm that test expectations match the
  intended behavior.
* For code or build changes, build Cloth using the
  `build instructions <../building/Building.rst>`_ and run relevant tests
  where available. For changes in compiler behavior, exercise the affected
  path with representative inputs and relevant edge cases. For example,
  run the built compiler with ``-L --src PATH`` for a lexer change and
  compare the printed token table with the expected tokens.
* For a bug fix, repeat the steps or reuse the input that reproduced the
  issue and verify the corrected behavior.

Submitting a pull request
-------------------------

If an agent drafted or materially shaped part of your change, describe
its role and the affected parts in the pull request. List the checks you
ran and their results. Include the commands and sample inputs reviewers
need to repeat the checks, and explain any relevant checks you skipped.
