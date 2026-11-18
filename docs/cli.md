# Command-line reference

Run commands from the source-tree root. In PowerShell, replace `ternfold` below
with `./build/ternfold.exe`, or put the executable on `PATH`.
`ternfold --help` prints the command summary.

## Source inspection and execution

```text
ternfold tokens source.tfd [--json] [--diagnostics-json] [--symbol-table DIR]
ternfold ast source.tfd [--json] [--diagnostics-json]
ternfold check source.tfd [--root DIR] [--diagnostics-json]
ternfold plan source.tfd [--root DIR] [--json] [--diagnostics-json]
ternfold core source.tfd [--root DIR] [--diagnostics-json]
ternfold run source.tfd [--root DIR] [--stdin-file FILE] [--trace NEW_FILE] [--diagnostics-json]
```

Options follow the source path, in any order, once each. The extension must be
exactly `.tfd`. Quote paths containing spaces. Unknown, duplicate, incomplete,
or command-inappropriate options fail with exit 2. Option values cannot start
with `-`; prefix a relative path with `./` when necessary.

`tokens` prints lexical tokens and spans. `--symbol-table DIR` also writes
`Symbol Table.txt` into an existing directory. No report is written by default.
`ast` prints the syntax tree, including recovery information for inspection.
Neither command resolves imports or checks types.

`check` resolves names and imports, checks types, and validates machine writers
and scheduling. Success prints `Frontend checks succeeded.`
`plan` prints machine dependencies and their deterministic schedule.
`core` lowers and verifies structured intermediate code, then prints it without
execution. `run` executes verified Core and writes the entrypoint's output.
Execution requires a root `pub fn main() -> List[Text]`.

`--root DIR` selects the local module-resolution root. See
[source management](source-management.md) for path and import restrictions.
`--json` selects token, AST, or plan schema v1 on stdout.
`--diagnostics-json` selects diagnostic schema v2 on stderr; driver failures
remain plain text. See [diagnostics](diagnostics.md).
Invalid source bytes produce an empty token result with `source_valid=false`,
or an AST with `root=null`, `has_errors=true`, and empty nodes.

## Input, output, and traces

When main has input clauses, `run` captures UTF-8 input from stdin, or from
`--stdin-file FILE`, before evaluation. Each input clause consumes one line.
Interactive use therefore requires EOF after the supplied lines. Missing lines
fault with R006. A main without input clauses does not read the input file.
Staged output clauses precede the final list's text values; each becomes a line.
Runtime faults discard staged program output. Diagnostics go to stderr.

`--trace NEW_FILE` records machine observations alongside normal execution.
It writes `NEW_FILE` and `NEW_FILE.origins`; both destinations must be new.
Use a fresh name for each run. Replay inspects an artifact without running code:

```text
ternfold run examples/intro/counter.tfd --trace build/counter.trace
ternfold replay build/counter.trace
```

Traces contain bounded previews and can report truncation. They describe private
emissions as well as acceptance, rejection, and faults. Replay is inspection,
not proof that the recorded execution occurred. See [trace format](trace-format.md).

## C emission and native builds

```text
ternfold emit-c source.tfd [-o FILE] [--opt 0|1] [--root DIR] [--diagnostics-json]
ternfold build source.tfd -o EXE [--cc PATH] [--runtime-root DIR] [--sanitize] [--opt 0|1] [--root DIR] [--diagnostics-json]
```

`emit-c` writes C17 to stdout unless `-o FILE` selects a file. Generated C uses
the project's runtime. `build` requires `-o EXE` and a compiler executable path
through `--cc PATH` or `TF_CC`. `--runtime-root DIR` points to this source tree;
its default is `.`. `--sanitize` requests a native sanitizer build.
`--opt 0` is the default; `--opt 1` enables Core optimization for these two
commands only. `run --opt 1` and `core --opt 1` are usage errors.

## Interactive and editor commands

`ternfold repl [--root DIR]` starts a local session. Enter declarations,
immutable bindings, or expressions. Use `:help`, `:type EXPR`, `:plan`,
`:load FILE`, `:reset`, `:cancel`, and `:quit` to inspect and control it.
`ternfold fmt source.tfd` writes canonical source to stdout without overwriting
the input. `--check` suppresses formatted output and exits 1 for differences.
`ternfold lsp [--root DIR]` serves framed JSON-RPC over stdin/stdout for one
open document, full-content updates, diagnostics, and type hover. See
[editor tooling](editor-tooling.md) for protocol capabilities and limits.

## Exit status

| Code | Meaning |
| --- | --- |
| 0 | Success, including a handled machine rejection |
| 1 | Diagnosed source error; also `fmt --check` formatting differences |
| 2 | Usage, host, resource, trace, or output failure |
| 3 | Runtime fault during execution |

In PowerShell, inspect `$LASTEXITCODE` immediately after the command.
