# Structured diagnostics — C17 M02

The compiler diagnostic bag in `include/ternfold/diagnostics.h` owns copies of
code, severity, message, labels, notes, help and edits. It borrows immutable
`TfSource` objects. Destroy diagnostic borrowers before their source manager;
the allocator/context outlives the bag. Returned records remain stable across
growth and failed additions. No compiler phase needs to print an error.

Additions validate codes, UTF-8 text, vector bounds, primary labels and spans.
Valid snapshots require scalar-boundary half-open byte ranges. Invalid snapshots
retain bounded raw-byte ranges with unavailable coordinates. Labels may overlap,
cross files or mark zero-width EOF. Messages/replacements have explicit lengths,
so embedded NUL is preserved. Source names follow the native source-path contract.

The cap counts retained records of every severity. Excess records increment
suppression while an error still sets `has_errors`, including at cap zero.
Invalid input/allocation failures leave prior records and status unchanged.
Lexer adaptation is transactional over the complete batch, including suppressed
lexical errors. This prevents a failed host allocation from publishing half an
error batch or making a failed source appear successful.

`TfDiagnosticPhaseResult` pairs a borrowed phase payload with its diagnostic bag.
A valid factory requires a non-null payload and no diagnosed errors, including
suppressed errors. The error factory carries no payload and requires diagnosed
errors. Warnings alone allow success. Successful diagnostic mutation invalidates
existing handles through a checked revision; failed transactional mutation leaves
them usable. Factories/accessors own nothing and cannot extend source, payload
or bag lifetime. This result is not a verified-Core execution capability.

Text output includes source snippets, separate labels, notes/help and proposed
edits. Coordinates count Unicode scalars; tabs advance to four-column stops.
Snippet escaping has its own displayed width, and no grapheme/terminal-width
claim is made. Color is an explicit caller policy, never inferred from ambient
terminal state. Neither diagnostics nor source snapshots are mutated by rendering.

JSON schema version2 is defined by the C header. It is deterministic insertion
order, not a claim of compatibility with the historical Python schema. The root
contains `schema_version`, `diagnostics`, `suppressed` and `has_errors`. Records
contain codes, severity, message, labels, notes/help and edits. Spans preserve
original start/end byte offsets and source text. Valid locations contain separate
one-based scalar and tab-display columns; invalid-source locations are null,
with raw bytes reversibly escaped. JSON never contains ANSI color sequences.

Maintained native tests independently decode generated JSON, then assert every
required schema field/type and decoded message, code, severity, source slice,
coordinate, label, note/help and edit value. This includes NUL, quotes, escapes,
Unicode, invalid raw bytes, cross-file labels and suppressed records; grammar
recognition or a visually plausible golden alone does not satisfy round-trip tests.

Stream writers borrow a binary `FILE*`. They report observed write failures;
the caller owns flush/close and errors detected only by flushing. Partial stream
output on host failure is possible; atomic file publication belongs to a driver.
The accepted standalone lexer/report interface and submitted package remain
unchanged. Later compiler commands consume these richer diagnostic APIs.
