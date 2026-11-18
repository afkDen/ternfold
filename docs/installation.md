# Installation

## Choose a download

Get a matching version from [GitHub Releases](https://github.com/afkDen/ternfold/releases).
For 0.2.0, the assets are:

| Asset | Use |
| --- | --- |
| `ternfold-0.2.0-source.zip` | Full C17 source, runtime, scripts, tests and examples |
| `ternfold-0.2.0-windows-x64.zip` | Ready-to-run CLI with `counter.tfd` and `hello.tfd` |
| `SHA256SUMS.txt` | SHA-256 checksums for both ZIPs |

The Windows CLI bundle runs the interpreter without a C compiler. Native
compilation needs a C toolchain and the full source release. Keep the source
tree intact: the native builder uses files under `runtime/`, `src/` and
`include/`. Copying only `runtime/` is insufficient.

## Verify and unpack

Use PowerShell 7 (`pwsh`) for the source build scripts. Download the ZIP and
`SHA256SUMS.txt` into the same directory. This example checks and runs the CLI
ZIP. To verify the source ZIP, change `$asset` and run through the hash check:

```powershell
$asset = 'ternfold-0.2.0-windows-x64.zip'
$entries = Get-Content -LiteralPath ./SHA256SUMS.txt
$entry = @($entries | Where-Object { $_ -match ('^[0-9a-fA-F]{64}  ' + [regex]::Escape($asset) + '$') })
if ($entry.Count -ne 1) { throw 'Expected one checksum entry' }
$expected = ($entry[0] -split '  ', 2)[0]
$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $asset).Hash
if ($actual -ine $expected) { throw 'ZIP checksum mismatch' }
Expand-Archive -LiteralPath $asset -DestinationPath ./ternfold-cli
Set-Location ./ternfold-cli
./ternfold.exe run ./counter.tfd
```

The example prints `4`. Extract each bundle into its own directory. For the
source ZIP, enter the enclosed `ternfold-0.2.0` directory before using the
commands below. A short local path such as `C:/src/ternfold` avoids long
compiler command lines; quote paths containing spaces.

## Build from source on Windows

The verified compiler setup is Windows x64, PowerShell 7 and LLVM-MinGW
Clang 23.1.4 with UCRT. `windows-toolchain.ps1` downloads the pinned
`llvm-mingw-20261020-ucrt-x86_64.zip`, checks its SHA-256, extracts it, and
returns the path to `clang.exe`. This provides the MinGW headers, libraries
and runtime as well as Clang. A standalone LLVM installation is a different
toolchain. The helper needs network access for the initial download; an
existing installation can be used offline.

Run these commands from the source root. Pass `$cc` explicitly because the
helper's default cache differs from the build wrapper's fallback cache.

```powershell
$cc = ./scripts/windows-toolchain.ps1
# Or set $cc to an existing LLVM-MinGW bin/clang.exe path.
./scripts/build.ps1 environment -Compiler $cc
./scripts/build.ps1 build -Compiler $cc
./build/ternfold.exe run examples/intro/counter.tfd
./build/ternfold.exe build examples/intro/counter.tfd -o build/counter.exe --cc $cc
./build/counter.exe
```

`environment` probes the selected host and compiler; `build` creates the
tools. To run the strict suite and the ASan/UBSan suite:

```powershell
./scripts/build.ps1 check -Compiler $cc
$env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1'
./scripts/build.ps1 sanitize -Compiler $cc
```

For a CLI bundle used outside the source root, pass the full extracted tree:

```powershell
./ternfold.exe build counter.tfd -o counter.exe --cc $cc --runtime-root C:/src/ternfold
```

`--runtime-root` defaults to the current directory. `--root` separately
controls Ternfold module containment; see [modules](module-system.md).

## Other platforms

The intended POSIX commands are `sh scripts/build.sh environment`, `build`,
`check` and `sanitize`, with `CC` selecting the compiler. Full compiler builds
on Linux and macOS, GCC/MSVC support, and Windows LeakSanitizer remain
unverified. See [troubleshooting](troubleshooting.md) for host limitations.
