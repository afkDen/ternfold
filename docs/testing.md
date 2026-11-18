# Testing the compiler

`scripts/build.ps1 check` builds with strict warnings and runs the lexer,
parser, checker, Core, runtime and command-line tests. The example manifest
checks output bytes and diagnostic codes. The differential tests run
generated native programs against the reference interpreter.

`sanitize` runs the same suite with AddressSanitizer and
UndefinedBehaviorSanitizer. Set `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`
on Windows; LeakSanitizer has not been verified on that host. Allocation
failure tests also check ownership and cleanup with counted allocators.

CI runs the strict and sanitizer suites in separate Windows jobs. Each job
also compares the interpreter and native opt0/opt1, and runs the composition
fixtures. Windows hosts that disallow unprivileged symlinks may report the
two capability skips; the regular alias checks still run.
