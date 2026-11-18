# Development history

The `development-history` branch reconstructs Ternfold's C17 implementation
from 35 existing local checkpoints. It starts with the source infrastructure
and lexer, then follows diagnostics, parsing, resolution, types, machine
planning, Core, execution, native code generation, and the later tools.

The reconstruction was committed when it was published. Each retrospective
commit records its original checkpoint ID and whether it contains source
or conformance changes. Source and tests are separate review steps, so some
intermediate commits are incomplete snapshots. Check out the branch tip or
a release tag to build the complete distribution.

Internal records, attachments and environment files are outside this
history. The old example manifest contained links to internal records;
the curated manifest appears with the completed distribution.

## Milestones

| Stage | Work |
| --- | --- |
| Foundation and M01–M03 | C17 source infrastructure, cached snapshots, diagnostic spans and token inspection |
| M04 | AST construction, handwritten parsing and syntax inspection |
| M05–M06 | Local modules, names, concrete types, constants and patterns |
| M07 and C01 | Machine dependencies and cycles; checked numerical frontend |
| M08 | Typed lowering, closures, exhaustive matches and verified Core |
| M09 | Reference execution through explicit frames |
| M10–M11 | Native values and ownership, heap call frames, structured C17 generation and native builds |
| M12–M14 | Source libraries, example corpus, transactional REPL, differential tests and release checks |
| M15–M17 | Executed traces and replay, optional optimization, formatter and local editor tools |
| X01 | Bounded composition study; keep explicit library composition in 0.2 |

The original course and delivery work is represented by C-MIGRATION-01,
C00, C01 and S01. M00–M14 form the core progression; M15–M17 and X01
were optional work after the core. The classroom lexer keeps its separate
[repository](https://github.com/afkDen/PPL_Group1_LexicalAnalyzer).

## CI records

The history is retrospective; it does not imply that GitHub Actions ran
when the original local checkpoints were created. Current runs belong to
the SHA shown on each Actions page.

- [Initial distribution](https://github.com/afkDen/ternfold/actions/runs/44288780575):
  strict and ASan/UBSan on `6ca52d2`.
- [Release verification](https://github.com/afkDen/ternfold/actions/runs/44289048019):
  strict, ASan/UBSan, fresh optimized packaging, and native execution for `v0.2.0`.
- [Checkout action update](https://github.com/afkDen/ternfold/actions/runs/44290217626):
  strict and ASan/UBSan on `68aa5b9`.

The repository keeps the original public import commits and the `v0.2.0`
tag. The reconstructed history joins them through a merge with no compiler
source changes.
