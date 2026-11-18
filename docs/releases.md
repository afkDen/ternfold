# Building a release

GitHub Actions checks pull requests, pushes to `main`, and version tags.
The checks use the pinned Windows x64 LLVM-MinGW toolchain. Linux, macOS,
Windows ARM64, GCC, MSVC and LeakSanitizer are outside the verified matrix.

Tag a checked revision as `vMAJOR.MINOR.PATCH`. The release job waits for
the strict and sanitizer jobs, tests an optimized compiler from a fresh
source ZIP, then uploads the source and Windows x64 command-line bundles
with `SHA256SUMS.txt`. A failed gate leaves the release unpublished.

To produce the same assets locally after creating a tag, use a short checkout
path such as `C:/work/ternfold`. The test suite creates nested paths, which can
exceed Windows path limits in a deep checkout.

```powershell
$cc = './toolchain/bin/clang.exe'
./scripts/release.ps1 -Tag v0.2.0 -Compiler $cc
```

The Windows bundle includes `ternfold.exe` and two programs to try.
The interpreter works without a C toolchain. To turn a Ternfold program
into a native executable, also get the source ZIP and install LLVM-MinGW.
Pass the unpacked source directory as `--runtime-root` if building a
program in a different directory. The classroom lexer and its installers
are maintained separately.
