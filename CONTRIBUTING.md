# Contributing

Branch from `main` and open a pull request. For a language change, describe
the programs whose behavior changes and update the specification with the
implementation. For a bug, include a short reproducer and the expected
output or diagnostic. Keep checked-in fixtures small.

Public document paths are listed in `scripts/public-documents.json`. Update
the inventory when adding or renaming a page.

```powershell
$cc = 'C:/toolchain/bin/clang.exe'
./scripts/build.ps1 check -Compiler $cc
./scripts/build.ps1 sanitize -Compiler $cc
```

Use the build wrapper for sanitizer tests on Windows. It adds the matching
toolchain DLLs to the child process search path. See [testing](docs/testing.md)
for the extra interpreter/native checks and the [release guide](docs/releases.md)
for the tag and packaging process.

Keep machine state immutable, use explicit ownership in C, and run both
execution paths when changing code that can affect runtime behavior. Build
products, trace captures, local editor settings and downloaded dependencies
belong outside commits.
