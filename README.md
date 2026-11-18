# Ternfold

[![Compiler CI](https://github.com/afkDen/ternfold/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/afkDen/ternfold/actions/workflows/ci.yml)

Ternfold is a statically typed language for programs that manage state. Its
compiler and runtime are written in C17. The same program can run in the
reference interpreter or compile to C and a native executable.

A machine transition starts from one immutable snapshot. It computes the
proposed state in dependency order, checks guards in source order, then
publishes the new state and emissions together. On rejection, the caller
gets the original state and a typed reason.

## Language and tools

| Area | Support in 0.2 |
| --- | --- |
| Values | Int, finite Float, Bool, Char, Text, Unit and homogeneous lists |
| Data modeling | Nominal records and enums, exhaustive pattern matching |
| Computation | Typed functions, closures, conditionals and pure list comprehensions |
| State | Immutable machines, explicit `next.cell` dependencies and typed rejection |
| Program boundaries | Local modules, explicit source-library imports and staged main I/O |
| Developer tools | REPL, token/AST/Core inspection, plans, traces, formatter and stdio editor service |

The language has no ambient I/O inside ordinary functions or machines. A
root `main` can capture input and stage output through its entry clauses.
See the [language specification](docs/language-spec.md) for the full rules.

```tfd
machine Counter(delta: Int) yields Int rejects Text {
    state value: Int = 0;
    next value = value + delta;
    guard next.value >= 0 else "negative count";
    emit next.value;
}

pub fn main() -> List[Text] {
    match step(Counter(), 4) {
        Decision::Accept(counter, values) => { [int_text(counter.value)] },
        Decision::Reject(counter, reason) => { [reason] }
    }
}
```

## Build and run

Get the source or the ready-to-run Windows x64 CLI from
[Releases](https://github.com/afkDen/ternfold/releases). The CLI bundle can
interpret programs without a C toolchain. Native builds use the toolchain
and the runtime sources from the source bundle.

The verified setup is Windows x64, PowerShell 7 and LLVM-MinGW Clang 23.1.4
(UCRT). Download the toolchain once or point the build script at an existing
installation. No network connection is needed to build with that installation.
Run `./scripts/windows-toolchain.ps1` to download the pinned toolchain and
check its SHA-256.

```powershell
$cc = 'C:/toolchain/bin/clang.exe'
./scripts/build.ps1 build -Compiler $cc
./build/ternfold.exe run examples/intro/counter.tfd
./build/ternfold.exe build examples/intro/counter.tfd -o build/counter.exe --cc $cc
./build/counter.exe
```

Both runs print `4`. To read the generated C, use `emit-c` instead of
`build`. Use `ternfold --help` for the REPL, formatter, editor service,
machine plans and transition trace commands.

## Check a change

```powershell
./scripts/build.ps1 check -Compiler $cc
$env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1'
./scripts/build.ps1 sanitize -Compiler $cc
./benchmarks/three-way.ps1 check -Compiler $cc
```

CI runs strict and ASan/UBSan jobs on Windows x64. The suites include
allocation failures, invalid source, diagnostic spans, generated programs
and interpreter/native comparisons. [Testing](docs/testing.md) explains the
checks and the distinction between a source rejection and a host failure.

## Platform status

The full compiler is verified on Windows x64 with LLVM-MinGW Clang 23.1.4
and UCRT. Linux, macOS, Windows ARM64, GCC and MSVC are unverified for the
full compiler. Windows LeakSanitizer is also unverified. The separate lexer
submission has its own platform results.

## Where to look

| Path | Contents |
| --- | --- |
| [docs/](docs/README.md) | Quickstart, language reference, compiler internals and release notes |
| [examples/](examples/README.md) | Small programs, complete machine examples and diagnosed errors |
| `src/`, `include/` | Compiler, interpreter and developer commands |
| `runtime/`, `stdlib/` | Native runtime and importable Ternfold source modules |
| `tests/` | Regression tests and fixtures used by the build |

See [CONTRIBUTING.md](CONTRIBUTING.md) for the checks run on changes. The
[classroom lexer](https://github.com/afkDen/PPL_Group1_LexicalAnalyzer) is a
separate repository with its own source and executable releases.

## Guides

- [Installation](docs/installation.md) covers the CLI bundle, source builds and checksums.
- [Tutorial](docs/tutorial.md) introduces expressions and machine transitions.
- [CLI reference](docs/cli.md) lists commands, options and exit codes.
- [Troubleshooting](docs/troubleshooting.md) covers build and runtime failures.
- [Development history](docs/development-history.md) follows the compiler milestones and their reconstruction.
