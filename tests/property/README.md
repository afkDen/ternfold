# M14 generated conformance

Run from the repository root. The executable takes
`COMPILER CC RUNTIME_ROOT [--sanitize] [SEED [CASE]]`. The default seed
is 141592653. Seeds are decimal uint32 values; cases are zero-based (0–17).
A single case is generated directly from the seed and index, independent of
earlier executions.

For integration, compile `tests/property/m14_generated.c` as a standalone
test with the same strict flags and compiler selection as
`tests/test_differential.c`. Pass the selected Ternfold CLI, C compiler and
runtime root; forward `--sanitize` in the sanitizer gate so the generated
native executables are also instrumented. The test includes the existing
differential translation unit under a renamed entry point to share its
argv-based launcher, capture limits, timeouts and private scratch handling.
Include this directory in the full compiler source package. The standalone
lexical package does not need it.

Each seed runs nine value cases and nine ordered fault cases. The value
families exercise bounded Int bindings, Int64 text endpoints, and accepting
or rejecting machines with shuffled writers, small acyclic candidate graphs
and old-state reads. The fault families cover overflow/underflow,
division/remainder by zero and negative Int powers in programs with staged
output and a competing later fault. Every case checks each engine against
the model, including raw stdout, exit status and exact runtime reports with
the first operator's source span. A typed generator cannot replace the
separate malformed-source tests.

On a semantic mismatch, replay the printed seed and case. The test saves
both versions and the original and reduced stream captures in its printed
scratch directory. A typed reducer tries removing bindings, reducing
literals, and deleting graph cells/edges. It keeps the value/fault family,
fault sites and guard policy, and accepts only candidates with the same
per-engine status, observed runtime code and output/report mismatch flags.
The reducer is greedy and stops after at most 24 probes; it does not promise
a globally smallest source. Unexpected static failures, resource failures
and timeouts fail the suite immediately and leave the active source and
captures for inspection.
