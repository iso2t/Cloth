.. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

RFC Process
===========

A request for comments (RFC) is a written proposal for a significant
change to the Cloth language or compiler. Use it to gather feedback
and agree on the design before merging the implementation.

When to write an RFC
--------------------

Write an RFC for any of these changes.

* Changing language syntax or semantics, including the type system,
  owner/operator model, or state rules.
* Changing public compiler behavior in ways that affect compatibility,
  such as changes to command-line flags, input formats, or exit codes.
* Making substantial changes to compiler architecture or interfaces
  shared across compiler components.

Use regular pull request review for routine bug fixes, small
behavior-preserving refactors, and documentation corrections.

Proposing a change
------------------

Post the proposal in a GitHub issue in the Cloth repository. Start the
title with ``RFC:`` and link related issues or prototypes.

* Describe the problem and who it affects.
* Explain the proposed design. For user-facing changes, include short
  Cloth examples or compiler invocations with expected results.
* Summarize the alternatives you considered and their tradeoffs.
* Explain how the change affects existing programs or tools.
* Identify affected compiler components, outline how to verify the
  change, and list open questions.

Discussion and decision
-----------------------

Discuss the proposal in the issue and update its description as the
design changes. Prototypes and draft pull requests can help resolve
open questions.

A maintainer records the decision and its rationale in the issue,
including whether the RFC is accepted, needs revision, is deferred,
or is declined. If revisions are requested, update the proposal in
the same issue and request another review.

Implementing an accepted RFC
----------------------------

Merge implementation changes after a maintainer records RFC
acceptance. Use regular pull request review for the implementation.
Link the accepted RFC in each pull request and include the tests and
documentation the change needs. Follow the `AI Agents <AI%20Agents.rst>`_
guidelines for AI-assisted changes.

If the implementation requires a material change to the accepted
design, bring it back to the RFC issue for discussion and a revised
decision before merging.
