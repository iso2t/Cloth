---
name: cloth-documentation
description: Write or update Cloth repository documentation and review whether readers can find it through the README, welcome page, and component guides.
---

# Cloth documentation

<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->

Use the [contributor guide](../../../Documentation/contributing/Documentation.rst)
for Cloth's page placement conventions and its worked example.

## Review the layout

Trace the reader's task from an entry page to the section that answers it.
For a review, identify where the path breaks and suggest a specific edit to
the link, heading, or destination. For a requested reorganization, check
incoming links to any paths and headings that move.

## Write or update a page

Edit the page that answers the reader's question. When writing a new page,
link it from the entry page that reader uses. Describe the behavior they
can observe and check it against the owning source and tests.

For Cloth examples, read the lit fixture's `RUN` lines to see whether it
checks the frontend, emits an object, or executes a program. Run the case
that exercises the documented result and verify changed links and anchors.
