# Source snapshots and compilation sessions

The standalone lexer keeps its accepted `TfSource` API. Compiler sessions add
`TfSourceManager` through `include/ternfold/source_manager.h`; they use the same
source bytes, indexes and span operations rather than a second source decoder.

The manager owns each source in a separate allocation. Returned managed entries,
sources and spans stay at stable addresses across cache growth. A manager-scoped
`TfSourceId` distinguishes sessions; lookup rejects another manager's ID, index
zero and out-of-range indexes. All borrowed entries expire at manager destruction.
Destroy lexer/parser/diagnostic borrowers first. Allocator callbacks and their
context must outlive the manager and every allocation it owns.

On first load the manager resolves a native path and snapshots its original bytes.
Canonical resolved paths identify files within the session. Successful request
aliases are retained; requesting one again returns its first snapshot without
silently rereading an edited/deleted file. A previously unseen alias may require
filesystem resolution before finding the already cached canonical entry. Hard
links need not collapse because module identity is a canonical path, not an inode.
The manager stores a SHA-256 digest of the retained original bytes, computed once.

Encoding errors are complete snapshots too: invalid UTF-8, a BOM or a lone CR
remain cached with structured source errors. Host/read/allocation failures publish
no incomplete entry and preserve previously returned snapshots. Such failed loads
may be retried. A manager is not thread safe and does not promise isolation from
concurrent hostile filesystem changes during the initial resolve/read operation.

Native path representation and adapters are documented in the public header.
Windows source-content UTF-8 does not imply universal UTF-8 filename support.
The current Windows adapter requires queryable case-insensitive ancestors for
both request and canonical target. It rejects unsupported case-sensitive or
unqueryable traversal as a host failure. Owned UTF-16 keys use ordinal comparison;
file IDs and linguistic collation never define module identity. This preserves
the first snapshot across case-only replacement as well as deletion. The initial
supported path policy remains part of cached aliases during a session. Native
read-attributes access to ancestors is required. If a host denies that access,
source resolution reports a host failure.
The API capability is documented by Microsoft's
[file-information classes](https://learn.microsoft.com/windows/win32/api/minwinbase/ne-minwinbase-file_info_by_handle_class)
and [per-directory case policy](https://devblogs.microsoft.com/commandline/per-directory-case-sensitivity-and-wsl/).
The source manager does not enforce `.tfd`, import grammar or root containment;
the driver and M05 module loader own those policies. It never prints diagnostics.
Original byte offsets, one-based scalar coordinates and tab-display coordinates
remain separate; invalid encodings have raw error ranges, not invented locations.

M01 acceptance requires cache, identity, lifetime, failure-injection and hash
assertions in addition to the existing source-coordinate/span corpus. Later
diagnostic and parser contexts borrow these same immutable session snapshots.
