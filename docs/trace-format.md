# Trace format and replay

Records describe observations, not an authenticated execution proof. Numeric
Core IDs require the exact original verified Core metadata for interpretation.
Inspection cannot verify writer coverage, guard truth, source identity, nominal
types, or that an event actually executed. A preview is counted explanatory
bytes, never an input value or executable code. Emission records always describe
private computed values; neither inspection nor serialization publishes them
as program output.

## Protocol version 1

Use binary-mode files (`rb`/`wb`) containing ASCII with LF line endings. No BOM,
CR, tabs, comments, additional whitespace, raw NUL or trailing bytes are accepted.
Every separator shown below is one ASCII space. Every integer is unsigned
canonical decimal: `0` or a nonzero digit followed by digits, without a sign or
leading zero. Booleans are `0` or `1`.

```text
TFTRACE 1
M count last_sequence last_invocation omitted_events clipped_previews omitted_preview_bytes truncated counters_saturated sequence_exhausted invocation_exhausted
R sequence invocation parent kind function block let span ordinal preview_truncated preview_length "preview"
END
```

There are exactly `count` record lines in a complete file. `END` also ends with
LF. The `M` fields preserve all recorder counters and flags; capacity and unused
storage are not persisted. The `R` fields preserve each retained record in array
order. No pointers, addresses, function names or host identities are emitted.

| Kind | Number | Meaning |
|---|---:|---|
| step_begin | 1 | Invocation starts; old/input preview |
| writer | 2 | A scheduled candidate completed |
| guard | 3 | A guard completed |
| emission | 4 | A private emission completed |
| accept | 5 | Accepted snapshot/emissions completed |
| reject | 6 | First false guard's rejection reason completed |
| fault | 7 | Invocation aborted; no publication |

Sequences and invocation IDs are nonzero `uint64_t`. Parent zero denotes a root;
otherwise it must be smaller than the invocation ID. Function, block, let, span
and ordinal are `size_t` on the inspecting host; values outside that host's range
are rejected before conversion. Numeric zero means absent for Core IDs. Zero
block/let IDs from executor hooks are valid. Event function IDs can differ from
the invocation's begin function, including a fault originating in a helper.
Record preview length is at most 64; it counts decoded bytes.

In quoted previews, bytes 0x20 through 0x7E appear literally except quote and
backslash. Every other byte, including quote and backslash, uses exactly `\xHH`
with uppercase hexadecimal digits. For example NUL is `\x00`, LF is `\x0A`,
quote is `\x22`, and backslash is `\x5C`. Escaping safe printable bytes,
lowercase hex, `\n`, and `\u` are rejected. This handles arbitrary bytes,
embedded NUL, incomplete UTF-8 and format-string characters without conversion,
accidental truncation or dependence on locale. The closing quote follows exactly
the declared number of decoded bytes.

For example, this complete invocation has an empty old/input preview and a
four-byte accepted preview containing `a`, NUL, LF, quote:

```text
TFTRACE 1
M 2 2 1 0 0 0 0 0 0 0
R 1 1 0 1 7 0 0 0 0 0 0 ""
R 2 1 0 5 7 0 0 0 0 0 4 "a\x00\x0A\x22"
END
```

## Validation and UNKNOWN

Serialization and replay both validate records, before publishing their result:

- Sequences strictly increase and do not exceed the recorded last sequence.
  Invocation IDs do not exceed the last minted ID. New begin IDs increase.
- Begins name the currently active parent. Non-begin records belong to the
  innermost active invocation and preserve its parent ID. No parent progresses
  until its child terminates; a child fault can only be followed by ancestor
  fault unwinding. Multiple independent root invocations are permitted.
- Begins and terminals have ordinal zero. Writers have distinct positive source
  cell ordinals, all before guards/emissions. Writer ordinals may occur in any
  order, because scheduling is not source declaration order.
- Guards have consecutive source ordinals starting at one, before emissions.
  Emissions have consecutive ordinals starting at one. Reject requires a prior
  guard as that invocation's immediately preceding event and no emissions.
  Accept/fault may terminate any appropriate phase; events after a terminal need
  a fresh begin. Guard previews are never interpreted as Booleans or code.
- Preview clipping counts equal retained clipped records in complete input;
  for an incomplete suffix they cannot be smaller than the retained count.
  Loss/clipping/exhaustion/saturation imply recorder truncation; byte loss
  requires a clipped preview. Exhausted ID flags require `UINT64_MAX` counters;
  saturation requires a saturated loss counter. Invalid combinations reject.

Replay requires the complete valid version and metadata lines. After that, EOF
without `END`, or inside a syntactically incomplete final record/`END` token,
produces a successful **UNKNOWN** inspection. It retains only complete record
lines. An invalid token, overflow, raw NUL, malformed complete line, early `END`,
extra record, or bytes after `END` rejects the entire inspection. A partial last
record is not treated as a completed terminal or writer. Incomplete metadata is
an error because bounds and IDs cannot yet be established.

`complete_file` means the declared records and `END\n` were received; it does
not mean every invocation terminated. `unknown` is true for an incomplete file,
any recorder truncation (including clipped previews), any invocation without a
terminal, missing minted invocation IDs, or sequence gaps/unobserved attempts.
All effective invocation `outcome` fields are conservatively `UNKNOWN` if the
inspection is unknown. `observed_terminal` separately preserves a retained
accept/reject/fault, without upgrading that partial inspection into a known
outcome. Missing terminals are never synthesized. Boundary-seeded high IDs
remain inspectable as UNKNOWN rather than overflowing or wrapping.

## API, ownership and bounds

`include/ternfold/trace_io.h` exposes:

- `tf_trace_serialize`: borrowed recorder to owned ASCII buffer and counted
  length, with a convenience final NUL excluded from the length.
- `tf_trace_write`: stage and validate, then write/flush to a borrowed stream.
- `tf_trace_replay_bytes` / `tf_trace_replay_read`: inspect borrowed bytes/stream
  into owned records and invocation summaries, sorted by invocation ID.
- `tf_trace_serialized_destroy` / `tf_trace_replay_destroy`: release output
  through its original allocator and zero it; NULL/zero owners are safe.

Use fresh, disjoint output objects. All failing constructors leave zero,
destroyable output. Inputs must designate live arrays of their stated lengths;
the C API cannot validate pointer provenance. Inputs/streams are never retained
or closed. Custom `TfAllocator` callbacks and context must outlive the outputs.
No runtime references are borrowed past the call. No process-global mutable
state, recursive traversal, subprocess, shell command, or host evaluation exists.

NULL limits select hard caps: **4 MiB serialized bytes and 4,096 records**.
Custom limits must be positive and no larger than those caps. The byte cap
excludes the convenience final NUL; exact-boundary files succeed. Decode allocates
one invocation array and one record array, sized from the validated declared
count (including a missing suffix). File read adds one `max_bytes+1` buffer,
reading the extra byte to detect oversized files. Encode allocates an invocation
validation array and one `max_bytes+1` staging buffer. Empty arrays allocate
nothing. There is no realloc. Array multiplications are checked before allocation;
buffer addition is bounded by the hard cap. Numeric conversion checks bounds
before multiplication/addition; count/ordinal increments are bounded by 4,096.
Only validated preview prefixes are read. Cleanup always releases partial arrays
and staging storage, including syntax/order/size/I/O and allocation failures.

Work is bounded by bytes plus records squared: distinct writer ordinals are
checked against prior records (at most 16,777,216 comparisons at the hard cap).
Nested invocation lookup uses binary search and explicit indices. Inspection
does not recurse even at 2,048 simultaneously active invocations.

Statuses distinguish invalid recorder/argument/metadata (`INVALID`), caller byte
or serializer record bound (`LIMIT`), allocation failure (`ALLOCATION`), wire
syntax/numeric-range/header-record bound errors (`SYNTAX`), invocation/phase
errors (`ORDER`), and read/write/flush failure (`STREAM`). Successful UNKNOWN
inspection returns `OK`; callers must inspect `unknown` and per-invocation
outcomes. Structural record ID/counter inconsistencies can return `INVALID`
from replay after syntax decoding. Size checks happen before dereferencing an
oversized input or allocating arrays.

No bytes are written for validation/allocation/size failure. Once host writes
start, an OS error may leave a partial trace file; `STREAM` reports this without
claiming rollback. Use a distinct trace sink, never program stdout. This API
does not perform path publication/rename or change the program's output/fault.
The host must handle eventual close failures separately. Partial replay owners
are inspection results; do not present their embedded recorder as a full capture.
