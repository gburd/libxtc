<!--
This directory is NOT prose.  Every .c file here is a code snippet that
appears (in whole or in named regions) in a documentation page under
docs/.  test/docs/test_doc_snippets.sh compiles and runs each one
against the freshly built static library; make check and CI run it, so
a snippet that stops building -- or a doc that drifts from the real API
-- fails the build.  Doc code is a release gate.

Conventions
-----------
- One self-contained .c per snippet, named NN_topic.c, mirroring the
  order it is introduced in the book.
- A snippet either has a main() that runs to completion with exit 0, or
  is a "fragment" (compile-only) marked with the first line comment
  `/* SNIPPET: compile-only */`.
- Regions a doc page embeds are delimited with
  `/* !region NAME */ ... /* !endregion NAME */` so a page can show just
  the interesting lines while the file still compiles and runs as a
  whole.  (The Jekyll include pulls the region; the test builds the
  whole file.)
- A snippet whose output a page shows has that output in NN_topic.out
  beside it.  The gate runs the snippet and diffs what it prints
  against the file; a page shows the file with
  `{% include output.html file="NN_topic.out" %}`.
- Pages include, they do not paste.  The gate fails on a ```c block in
  docs/ or README.md unless the line above it is `<!-- fragment -->` or
  `<!-- snippet: NN_topic.c -->` (a copy of the file's "full" region,
  for README.md and docs/getting-started.md, which forges render as
  plain Markdown).
- Snippets use ONLY the public xtc_* API (same rule as examples/): the
  api-discipline gate does not scan here, but the book must model good
  consumer code, so keep it clean by hand.
-->
# docs/snippets

Tested code for the documentation.  See the comment at the top of this
file.  Build+run locally:

```sh
XTC_SRC_DIR="$PWD" XTC_BUILD=build_unix sh test/docs/test_doc_snippets.sh
```
