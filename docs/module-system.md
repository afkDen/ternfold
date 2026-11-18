# C17 implementation target (ADR0011)

# Modules and program roots

Version 0.1 has local files and an explicitly selected root, not a package registry. `import "models/bin.tfd" as bin;` loads a file relative to the importing file's directory. The path must be a nonempty relative POSIX-style path ending `.tfd`; reject absolute paths, backslashes, NUL, URL schemes, and any `..` component. Resolve symlinks and verify the resolved file remains under the resolved `--root` directory. By default `--root` is the entry file's parent. Check the entry itself is under root. These rules avoid accidental imports outside the requested project; they do not promise protection against a concurrent hostile filesystem changing symlinks between checks and opens.

Canonical resolved paths identify modules within a compilation. SourceManager reads each once, stores original bytes and a content hash, and all phases use that snapshot; never reread halfway through a build. Two aliases may refer to one module, but duplicate alias names are E202. Import graph cycles are E205, diagnosed with a path chain even if declarations would otherwise permit mutual recursion. Imports must appear before declarations; no re-exports, wildcard imports, implicit relative-name imports, executable initialization, or online lookup.

`bin::Bin()` references a public machine constructor; `bin::Fault::Full` a public enum variant; `bin::helper(x)` a public function. Unqualified imports are absent. Private names are visible only in their defining module. The compiler-known prelude is available in every module; `stdlib/` source modules are ordinary local modules until a separate packaging ADR establishes an installable standard-library search location. Do not silently add magic path search to the compiler.

Check resolves signatures for every loaded module in dependency order before bodies. Top-level forward function references and mutual recursion within one module are allowed. A public record/enum/machine exposes its fields/variants/cells and must not use private types in any exposed position. Public function parameter/result types and machine input/output/rejection/state types are exposed positions. No separate compilation ABI is promised: v0.1 uses whole-program compilation, with TypeIds mangled deterministically from module-relative path and declaration index.

`check file.tfd` accepts library files without an entry. `run`/`build` require `pub fn main() -> List[Text]` in the chosen root module, E208 otherwise. All reachable imported modules are checked even if unused. Unreachable functions are also type checked; backend dead-code removal may happen only after semantic validation.

## C17 resolution interface

`tf_resolve` builds the module graph and lexical reference tables using a
borrowed SourceManager. The result owns the module parses, diagnostics, symbol
and nominal identity tables, scopes and captures. Destroy it before the source
manager. Resolved IDs are scoped to the result; module order is based on
canonical root-relative paths. Nominal declaration identity is distinct from
the concrete type representation of M06.

Earlier source or syntax errors leave their diagnostics available but prevent
the resolution phase from publishing a valid payload. On host or allocation
failure the result is null; successfully loaded snapshots may remain cached.
The checker does not reread a cached alias or expose a recovery tree to a later
compiler phase.

The driver command `ternfold check source.tfd [--root DIR] [--diagnostics-json]`
checks through machine analysis. Named arguments and field projections need
type information and are bound in M06. A root public main containing I/O is host-entry-only, as specified in
[ADR 0009](decisions/0009.md); an ordinary source reference to that symbol is E208.
