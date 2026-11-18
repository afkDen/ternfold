# C17 compiler pipeline

All implementation stages target ISO C17 (ADR 0011).
Source → tokens → AST → resolved program → typed program → machine plan →
verified Core → interpreter / structured C17 backend + runtime.

Current executable: `build/ternfold-lexer[.exe] source.tfd [--output-dir DIR]`.
It accepts only exact .tfd extensions, snapshots/validates UTF-8, scans using
character transitions, emits structured lexical diagnostics and atomically
publishes Symbol Table.txt. Exit 0 valid, 1 source/lexical error, 2 usage/host error.
The output directory must already exist. Source and report must not alias.

AST inspection, static type checking, machine analysis, verified Core and a
reference `run` command are available in the private compiler. Native
compilation and the transactional REPL are accepted on the documented Windows
host. Each command stops at its declared phase and refuses to continue after errors.
Only the verified-Core handle can enter either executor. Rich inspection output
must stay distinct from the exactly two-column classroom lexical report.

## Compiler token inspection — M03 accepted

`build/ternfold[.exe] tokens source.tfd [--json] [--diagnostics-json]
[--symbol-table DIR]` uses the existing character-DFA scanner, SourceManager
and M02diagnostic bag. TokenJSONschema1 includes comments/EOF, exact raw lexemes,
decoded literal payloads, reserved flags, original byte ranges and scalar/display
coordinates. Numeric strings are not range-converted before type checking.
Text is default; diagnosticsJSONschema2 goes to stderr when requested. Invalid
source snapshots have an empty token array and raw source diagnostics, without
invented coordinates. Exit0 lexicalsuccess,1 source/lexical/extension error,
2 usage/host/output failure. No Symbol Table is written unless explicitly requested.
The public standalone submission remains independent of the private compiler.

## Source checking and machine plans — M05 through M07

`build/ternfold[.exe] check source.tfd [--root DIR] [--diagnostics-json]` loads the
canonical local module graph and resolves lexical names before checking type
signatures, expression bodies, closed state initializers and match coverage.
It checks library roots without requiring a main. Diagnostics go to stderr;
exit 1 means a diagnosed source error, 2 means a usage or host failure.
It also validates machine writer coverage, lexical dependencies and cycles.
The `plan` command runs the same frontend and exposes the valid machine
schedules; see [machine plan inspection](machine-plan.md).

## Core inspection — M08

`build/ternfold[.exe] core source.tfd [--root DIR] [--diagnostics-json]`
runs the same static checks, lowers the typed machine plan, verifies the
frozen Core, and renders it as text. A diagnosed source error produces no
Core. A lowering, verification, or stream failure exits 2. See the
[Core contract](core-ir.md) for scope, type and ownership invariants.

## Reference execution — M09

`build/ternfold[.exe] run source.tfd [--root DIR] [--stdin-file FILE]
[--diagnostics-json]` requires `pub fn main() -> List[Text]` in the root.
It runs verified Core using explicit frames. An entry with input clauses
receives a UTF-8 snapshot from stdin or `--stdin-file`. Program stdout
contains completed Text lines followed by LF, preserving embedded NUL.
A language fault prints diagnostics and exits 3 without program stdout.
Source diagnostics exit 1; usage, host and output errors exit 2.

## Native compilation — M11

`build/ternfold[.exe] emit-c source.tfd [-o FILE] [--root DIR]
[--diagnostics-json]` writes an inspectable C17 translation unit. Without
`-o`, C goes to stdout. `build/ternfold[.exe] build source.tfd -o EXE
[--cc PATH] [--runtime-root DIR] [--sanitize] [--root DIR]
[--diagnostics-json]` uses a local GCC/Clang-style compiler, supplied as an
executable path with `--cc` or `TF_CC`. The runtime root defaults to the
current directory and must contain the distributed `runtime/`, `include/`,
and `src/` trees. The driver invokes the compiler with separate arguments,
retains a bounded excerpt of its diagnostics, and publishes only a complete
output. The output's parent must exist. An existing output is preserved when
source checks or the downstream C build fail.

Both commands require a runnable root `pub fn main() -> List[Text]` (E208).
Generated code enters only verified Core functions via the runtime's
continuation dispatcher. The resulting executable accepts `--stdin-file FILE`
and has the same stdout and language-fault exit contract as `run`. Native fault
diagnostics include the original source path and raw byte range, independent of
the generated C line numbers. Windows executable outputs should use `.exe`.

## Transactional session — M13

Optional transition tracing is tracked by M15 and
[ADR 0013](decisions/0013.md). Its bounded recorder is an internal building
block. No user-facing execution trace or replay command is implemented yet.

The `ternfold repl [--root DIR]` uses ordinary Ternfold
declarations, expressions and `let` bindings. Results are bounded debug lines;
errors are reported on stderr. Prompts are shown when input and output are
terminals. The shell retains lines for open delimiters or a grammar error
that occurs only at the end of the token prefix. Text and Char literals cannot span
physical newlines. A declaration may include several mutually recursive functions
in one submitted unit. Session `let` bindings are immutable, in one lexical
scope, and visible to later expressions but not to module-level functions.

- `:type EXPR` checks one expression on that line and prints its concrete type
  without evaluating it.
- `:plan` prints the session module's checked machine plan.
- `:load FILE` checks a local module and its imports, then replaces the session
  and clears the binding scope on success. It never runs the module's main.
- `:reset` clears declarations, bindings, and pinned imports. A standalone
  `:cancel` discards pending input; a standalone `:quit` discards it and exits.
  Those two shell commands work even at the continuation prompt.

An expression, closure, or machine decision executes only through verified Core.
Each evaluation replays the committed immutable binding prefix from pinned
source; it has the reference executor's normal budgets for the whole replay.
This preserves values without retaining arena pointers or stale type IDs. A
successful decision to reject is a value, and its reason and old snapshot
are inspectable. A runtime fault or failed load leaves the session unchanged.
The shell accepts up to 64 KiB per submitted unit, scans at most 32 MiB of
cumulative prefix bytes per multiline unit, and caps assembled root source
at 1 MiB and source-cache allocations at 64 MiB. The displayed virtual file is
`<repl>`; imports retain original source paths and imported nominal types in
`:type` use their module-relative path. Source I/O remains restricted to host entry.
