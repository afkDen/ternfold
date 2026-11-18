# C17 machine analysis and inspection — M07

See [release support](releases.md) for verified platforms.

The machine analysis phase consumes a valid borrowed `TfTypedProgram`. It
owns immutable plans and a diagnostic bag. Destruction does not destroy the
typed program, resolution, type store or sources; those owners must outlive
the plans. Allocation failures and checked-size failures publish no owner.
An erroneous phase carries no lowering payload or partial schedule.

Cells retain state declaration order and source identities. Writers, guards
and emissions retain written order. Reads retain their old/input/candidate
kind, source node and byte span, clause context, and target identity. Rejection
reads belong to the guard's false branch. These are lexical facts, not an
execution trace or a claim that every expression runs.

Exact writer coverage is a prerequisite to graph construction. For each valid
writer, every lexical `next.cell` occurrence contributes a dependency from
the prerequisite cell to the written cell, including occurrences in dead
branches and comprehensions. Repeated reads retain separate occurrence spans
on one unique graph edge. Old and input reads do not add edges. Guard,
rejection and emission reads do not reorder writers.

Cycle analysis is iterative and reports E404 with participating projection
spans and help distinguishing old-state reads from candidate reads. The
schedule uses Kahn's algorithm, choosing the smallest ready name by ASCII
bytes after every removal. A newly ready uppercase name can therefore precede
a lowercase name that was already waiting. No candidates are evaluated by
this phase. Guard truth is a runtime condition.

## Inspection contract

`ternfold plan source.tfd [--root DIR] [--json] [--diagnostics-json]` validates
the source through machine analysis and renders only a valid plan. Invalid
programs have no plan on stdout; diagnostics are on stderr. Exit codes are
0 success, 1 diagnosed source error, and 2 usage, resource or output failure.
`check` performs the same frontend validation without a plan dump. Neither
command executes Ternfold or emits C.

The allocation-free renderer borrows the immutable program and caller's
stream. The caller selects binary mode, flushes and closes. An observed
write error returns a host failure and can leave a prefix. Invalid phase
handles are rejected before output begins.

Text output lists schedules, cells, dependency occurrence indexes, lexical
reads, and guards/emissions in order. It explicitly states that guards are
runtime conditions.

JSON schema 1 has `schema_version`, `guards_proven: false`, `modules` and
`machines`. Module entries map one-based IDs to canonical source paths. In
the decoded `path` string, each code point U+0000–U+00FF is one original
native path byte. Interpret the recovered bytes using the host's path
convention (Windows ANSI code page, or POSIX native bytes). This mapping
preserves distinct native paths, even when some bytes look like UTF-8.
A machine contains its module,
declaration node, name, span, input symbol, cells, writers, schedule, reads,
dependencies, guards and emissions. Cell IDs are zero-based within that
machine. Symbol/type IDs are program-local; node IDs are module-local. Spans
are original half-open byte offsets.

A dependency has `prerequisite`, `dependent` and `occurrences` indexes into
the read table. Reads include `kind`, `context`, `clause_index`, `symbol`,
`node`, `cell` and `span`; input reads have a null cell. Each clause records
its node, expression and span. Guards additionally record the rejection node
and rejection span, evaluated only if the corresponding predicate is false.
The emission array preserves source order and does not publish values.
