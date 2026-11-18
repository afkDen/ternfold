# Formatter and local editor adapter

M17 integration is in progress under [ADR 0014](decisions/0014.md).
The tools reuse the production scanner, parser and checker. They do not execute
source, add syntax or require a hosted service.

`ternfold fmt source.tfd` writes canonical source to stdout; the input is
never overwritten. `--check` exits 0 if it is already canonical, 1 for
formatting differences or diagnosed source errors, and 2 for host failures.
Every literal, comment and token spelling is preserved. Whitespace changes use
LF and four-space brace indentation. Minus/numeric adjacency is retained,
including the minimum Int literal rule. Relexing and reparsing must validate
the candidate before any formatted bytes are returned.

`ternfold lsp [--root DIR]` serves a binary stdio JSON-RPC/LSP subset:
initialize, shutdown, exit, one open document, full-content changes, close,
diagnostics and type hover. Protocol output contains only framed messages.
Use a local client with full-document synchronization. Each accepted revision
owns an immutable source snapshot and ordinary compiler results; stale versions
cannot publish diagnostics or hover. UTF-16 conversion exists only in this
adapter and rejects surrogate-interior/CRLF-interior positions.

Bounds are explicit: 1 MiB message/source, 4,096 header bytes, 1,024 JSON
tokens, depth 32, and 2 MiB response. Imports resolve under the configured
local root; the virtual document does not provide an unsaved-file overlay for
other imports. The adapter is synchronous. Library cancellation and queued
request IDs suppress later publication, but a running compiler call cannot
process a newly arriving cancellation until it returns. These limits are part
of the supported protocol rather than an asynchronous editor claim.
Root plus dependency source bytes share the 1 MiB cap. Each snapshot has a
32 MiB live allocation budget; old and new snapshots can coexist during
transactional replacement. Document versions are signed. The cancellation
queue holds 32 distinct IDs and fails explicitly on overflow. Deep type hover
uses a bounded ellipsis rather than terminating the protocol.

See [testing](testing.md) for the formatter and protocol checks.
