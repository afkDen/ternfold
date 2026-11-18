# Learn Ternfold by running it

Start with the executable built in [quickstart](quickstart.md). Save each code
block below as its own `.tfd` file. From the source-tree root, run it with
`./build/ternfold.exe run FILE.tfd` in PowerShell.

## Values and functions

```tfd
record Label { name: Text, score: Int }
fn square(n: Int) -> Int { n ^ 2 }
pub fn main() -> List[Text] {
    let scores = for n in range(0, 4) { square(n) };
    let label = Label(name: "squares", score: len(scores));
    let first = match at(scores, 0) {
        Option::Some(value) => { int_text(value) },
        Option::None => { "empty" }
    };
    [label.name, int_text(label.score), first]
}
```

Save this as `values.tfd`; it prints `squares`, `4`, and `0` on separate lines.
`let` binds an immutable value. Functions declare parameter and result types;
local bindings may infer theirs. A block ends with an expression without a
semicolon. `for` constructs a list, and `range(0, 4)` excludes its upper bound.
Functions and comprehensions perform pure computation.

Records use named constructor arguments and dot projection. `at` returns an
`Option`, so the match handles both a present element and an absent one.
Lists have one element type. Text uses double quotes and `++` concatenation;
`'T'` is a Char, and `()` is Unit. `/` produces Float; `div` is floor integer
division, so `-7 div 3` is `-3`. Int arithmetic checks overflow.

## An immutable Counter

```tfd
machine Counter(delta: Int) yields Int rejects Text {
    state value: Int = 0;
    next value = value + delta;
    guard next.value >= 0 else "negative count";
    emit next.value;
}
pub fn main() -> List[Text] {
    let old = Counter();
    let accepted = match step(old, 4) {
        Decision::Accept(counter, values) => {
            [int_text(old.value), int_text(counter.value)]
        },
        Decision::Reject(counter, reason) => { [reason] }
    };
    let rejected = match step(old, -1) {
        Decision::Accept(counter, values) => { ["unexpected"] },
        Decision::Reject(counter, reason) => {
            [int_text(counter.value), reason]
        }
    };
    accepted ++ rejected
}
```

Save as `counter.tfd`. The output is `0`, `4`, `0`, then `negative count`.
`Counter()` creates the initial snapshot; `delta` is the argument to `step`,
not to the constructor. Both calls start from `old`, which stays at zero.
The accepted result contains a new snapshot and a list of emitted Int values.
The rejected result contains the unchanged old snapshot and its Text reason.
To continue from an accepted state, pass that returned snapshot to another step.

Within the transition, bare `value` reads the old snapshot, while `next.value`
reads the proposed value. All candidates finish before guards run in declaration
order. The first false guard selects the rejection reason. Emissions evaluate
only after guards pass and publish together on success. Arithmetic or resource
faults abort execution; they do not produce `Decision::Reject`.

## Candidate dependencies

```tfd
machine Scale(delta: Int) yields Int rejects Text {
    state a: Int = 0;
    state b: Int = 0;
    next b = next.a * 2;
    next a = a + delta;
    emit next.b;
}
pub fn main() -> List[Text] {
    match step(Scale(), 7) {
        Decision::Accept(snapshot, values) => { [int_text(snapshot.b)] },
        Decision::Reject(snapshot, reason) => { [reason] }
    }
}
```

Save as `dependencies.tfd`. `run` prints `14`. Use
`./build/ternfold.exe plan dependencies.tfd` to inspect the schedule: the lexical
read `next.a` requires writer `a` before `b`, despite their source order.
Every cell needs exactly one writer; missing writers, duplicates, and cycles
are errors. Among ready cells, the scheduler chooses ASCII name order.
Changing `next.a` to bare `a` reads zero from the old snapshot instead.

## Staged main input and output

```tfd
pub fn main() -> List[Text] {
    input visitor: Text from stdin;
    let greeting = "Hello " ++ visitor;
    output greeting to stdout;
    ["done"]
}
```

Save as `greet.tfd`. Run `"Ada" | ./build/ternfold.exe run greet.tfd` in
PowerShell, or use `run greet.tfd --stdin-file names.txt` with a UTF-8 file
containing `Ada` and a newline. Output is `Hello Ada`, then `done`.
The phrases `from stdin` and `to stdout` are optional. Input comes from a
captured snapshot; output stays private until the whole entry evaluation succeeds.
A later fault discards the greeting too. Each input clause consumes a line;
too few lines cause R006.

Only the outer body of root `pub fn main() -> List[Text]` permits these I/O
clauses. A main containing them is entry-only: calling or capturing it from
source is E208. A main without I/O clauses remains callable. Use
`check FILE.tfd` for diagnostics and [the CLI reference](cli.md) for native
builds, traces, formatting, and interactive sessions.
