# Troubleshooting

Run `ternfold --help` for the options supported by your executable. In
PowerShell, read `$LASTEXITCODE` immediately after the command.

| Exit | Meaning |
| --- | --- |
| 0 | Success; trace replay may still report `UNKNOWN` |
| 1 | Diagnosed source error; also a formatting difference with `fmt --check` |
| 2 | Usage, host, resource, native compilation or output failure |
| 3 | Runtime fault during interpreted execution |

`--diagnostics-json` writes diagnostic schema v2 to stderr. Driver failures
remain plain text, prefixed `ternfold:`. Normal command output uses stdout.

## Source and command errors

- `source file extension must be exactly .tfd`: rename the source with the
  lowercase suffix. `.TFD` and `.tfd.txt` fail with exit 1.
- `unknown, duplicate or incomplete option; use --help`: put options after
  the source path, use each once, and supply the required value (exit 2).
- `E001`, `E002`, `E003`: save as UTF-8 without a BOM, using LF or CRLF.
  These codes respectively identify invalid UTF-8, a BOM and a lone CR.
- `E208` with `run requires a root pub fn main() -> List[Text]`: add that
  entry signature in the selected entry module. `check` accepts libraries.
  I/O-bearing root `main` cannot be referenced by ordinary source calls.
- `source differs from canonical formatting`: `fmt --check` exits 1 when
  formatting would change. Plain `fmt` writes the result to stdout and
  leaves the input untouched. Save to a separate file before replacing it.

## Imports and Windows paths

`E204` with `import requires a relative POSIX .tfd path without parent traversal`
means the import needs a relative path such as `models/bin.tfd`. Imports
resolve from the importing file's directory. Use `/`; absolute paths,
backslashes, URLs and `..` components are rejected. `E205` reports an import
cycle; remove the cycle rather than changing declaration order.

`module escapes the canonical source root` is also `E204`. The default root
is the entry file's parent. Use `--root DIR` to select a common ancestor
containing the entry and all imports. Symlink targets must remain inside it.
There is no package registry or implicit `stdlib/` search path.

`cannot resolve source (read/path capability, resource limit or allocation failure)`
is a host failure (exit 2). Check file access and the chosen root. Windows
paths use the native ANSI code page and require queryable, case-insensitive
ancestors. Try a short local path representable in that code page. UTF-8
source content does not imply support for every Unicode filename. Avoid
changing system permissions or directory case policy to work around a failure.

## C compiler, runtime sources and DLLs

`build requires -o EXE` and `build requires --cc PATH or TF_CC` are exit 2
failures. Supply an output filename and a compiler executable path; `--cc`
takes one path, without embedded flags. For `native build failed`, read the
preceding compiler or filesystem diagnostic. Use the pinned LLVM-MinGW UCRT
toolchain described in [installation](installation.md).

Pass `--runtime-root` pointing to the full source tree when building from
another directory or using the CLI bundle. Create the output parent directory
first. `cannot create a temporary C17 file beside the output` means that
directory could not accept the staging file. The destination must also be a
regular file or a new path, and cannot alias a loaded Ternfold source.

A missing `libc++.dll` or another toolchain DLL when directly launching a
sanitized scratch executable is a Windows loader failure. Run the tests through
`./scripts/build.ps1 sanitize -Compiler $cc`; the wrapper prepends the selected
compiler's `bin` directory to PATH for its children. Keep any manual PATH
adjustment within the current process. Do not copy arbitrary DLLs into system
directories or change machine-wide PATH. Windows LeakSanitizer is unverified;
CI sets `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` in its test process.

## Input, traces and editor service

`R006` / `not enough input lines` exits 3. Supply the required lines through
stdin or `run source.tfd --stdin-file input.txt`. `input snapshot is not valid
UTF-8` is a host input failure (exit 2). Other runtime codes include `R001`
(overflow/nonfinite result), `R002` (zero divisor), `R003` (resource limit),
`R004` (unexpected interpreter state) and `R005` (negative Int exponent).

`cannot write new trace artifact and source map` exits 2. Choose a fresh
`--trace` path in an existing writable directory; both it and its `.origins`
sidecar must be new. Preserve existing artifacts. Replay's `UNKNOWN` means
the observations are incomplete or truncated; exit 0 does not establish a
complete execution. See [trace format](trace-format.md).

`lsp` waits for binary stdio JSON-RPC messages with `Content-Length` framing.
Connect an LSP client; typing JSON lines into a terminal is not the protocol.
`editor protocol or compiler host failure` exits 2. The service supports one
open document and full-content changes, with a 1 MiB message/source cap. Keep
logs off its protocol stdout. See [editor tooling](editor-tooling.md) for limits.
