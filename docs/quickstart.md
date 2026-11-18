# Run your first machine

Place the source tree and a Windows x64 LLVM-MinGW toolchain on disk. Pass
the full path to the toolchain's `clang.exe`:

```powershell
$cc = 'C:/toolchain/bin/clang.exe'
./scripts/build.ps1 environment -Compiler $cc
./scripts/build.ps1 build -Compiler $cc
./build/ternfold.exe plan examples/intro/counter.tfd
./build/ternfold.exe run examples/intro/counter.tfd --trace build/counter.trace
./build/ternfold.exe replay build/counter.trace
```

The plan shows which cells will be computed and in what order. The trace
shows the events from that run. Choose a new trace filename each time; the
compiler writes a second file with the suffix `.origins` and preserves
existing files.

Build the native version with `build examples/intro/counter.tfd -o
build/counter.exe --cc PATH`. Its output is the same as `run`. If you are
working from another directory, add `--runtime-root` with the path to this
source tree.

Try [the inventory example](../examples/showcase/branching_inventory.tfd)
next. It takes two different transitions from the same starting state.
