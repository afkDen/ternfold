# C17 parser and syntax inspection

M04 consumes the accepted handwritten lexer's token sequence. Recursive descent
handles declarations, type syntax, patterns, blocks and machine sections; Pratt
parsing handles expressions. It does not resolve names, check types, evaluate
constants, schedule machines or execute code. Semantic-invalid examples still
parse successfully. The dedicated vocabulary inventory is lexical-only.

`TfParseResult` owns an immutable AST arena, diagnostic bag and phase handle.
The source snapshot is borrowed and must outlive the result. Names, decoded
Text/Char values and node lists are copied into the arena, so the lexer result
may be destroyed immediately after parsing. Node IDs combine owner identity
and a stable one-based index; zero means absent. A foreign ID cannot resolve.
All nodes and payloads keep original source spans; negative literal patterns
retain the minus span separately. Allocation failure destroys the entire result
and returns HOST_ERROR with a null output. No mutable compiler globals exist.

`tf_parse` returning OK means a complete owning result, including a recovery
tree on diagnosed syntax errors. A VALID phase handle carries the AST; an ERROR
handle carries no successful payload. `tf_parse_ast` permits inspecting the
recovery tree only. Neither constitutes a verified Core execution capability.
Missing syntax becomes zero-width MISSING nodes; ERROR nodes retain skipped
ranges. Recovery respects nesting and following declaration boundaries and must
consume a token or return. Lexer diagnostics share the parser bag without adding
a second E110 for a lexical INVALID token. The default diagnostic cap is100.

The nesting budget defaults to64 and is configurable from1 through128.
Exceeding it is a host resource failure, not a valid parse or a language error.
Production input must be an unmodified lexer result for a valid source snapshot;
the parser checks token span/order/EOF/payload invariants before use.

## CLI

`ternfold ast source.tfd [--json] [--diagnostics-json]` writes syntax inspection
to stdout and diagnostics to stderr. Options follow the filename once each.
It writes no Symbol Table. `--symbol-table` belongs to the tokens command only.
Exit0 means syntax success,1 diagnosed source/lexical/syntax error,2 usage,
host/resource or output failure. `check`, `run` and `compile` stay unavailable
until their owning milestones implement them.

AST JSON schema1 has `schema_version`, `root`, `has_errors` and `nodes`. Nodes
are a flat array in stable ID order with `id`, `kind`, original `start_byte` /
`end_byte` and kind-specific syntactic fields. References are numeric IDs or
null; ordered lists preserve source order. Text/Char payloads are decoded UTF-8
with JSON escaping including embedded NUL. Literal token kinds preserve numeric
spelling. Text output has a summary and one node record per line. Neither format
contains host addresses; repeated dumps of unchanged input are byte-identical.
Writers allocate nothing and borrow the binary stream; callers check flush/close.
Observed stream failures propagate, though a failed stream may contain a prefix.

Invalid source encoding/line endings cannot produce an AST: JSON uses a null
root, has_errors=true and an empty nodes list, with raw-source diagnostics on
stderr. Syntax-error trees are intentionally available for recovery inspection.

I/O clauses are syntactically admitted only in the outer body of a root public
zero-argument main. Invalid contexts, including machine clauses, diagnose E208.
A main without I/O parses ordinary calls. The approved identity-based rejection
of references to an I/O-bearing main belongs to M05 resolution; host execution
belongs to later verified Core/executor milestones.
