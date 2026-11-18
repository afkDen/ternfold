# Composing machine transitions

A Ternfold function can call two machine steps, inspect both decisions,
and return both proposed states only when both steps accept. The caller
selects which snapshots and emissions to return using ordinary expressions.
The [checkout example](../examples/showcase/checkout.tfd) uses this pattern
to combine stock and wallet transitions.

The bounded experiment compares explicit composition with a flattened
schedule that runs all candidates before any guard or emission. For total
operations within the tested domain, the two policies agree. With faults,
they can report different first errors: a child may divide by zero in
an emission before a later child reaches an overflowing candidate. The
flattened schedule encounters the overflow first.

Ternfold 0.2 leaves composition to library code. It has no joint-step syntax
or static proof rules. See [ADR 0016](decisions/0016.md) for the decision.
