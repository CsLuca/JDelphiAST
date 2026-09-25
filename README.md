# JDelphiAST

JDelphiAST is an experimental C++20 Delphi/Object Pascal dependency analyzer. It parses source units, indexes exported symbols, resolves unqualified references through visible `uses` clauses and classifies each declared dependency as `USED`, `UNUSED`, `SIDE-EFFECT`, or `UNKNOWN`.

The analyzer is deliberately conservative: unavailable source and ambiguous symbol ownership never produce an automatic removal recommendation.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Usage

Pass both the unit under analysis and the source units needed to build the symbol index:

```sh
jdelphiast_cli MyPlugin.pas System.SysUtils.pas System.Classes.pas
jdelphiast_cli --json MyPlugin.pas System.SysUtils.pas System.Classes.pas
```

Text output includes removal recommendations only for known, side-effect-free units classified `UNUSED` with `HIGH` confidence. JSON output uses schema version 1 and includes byte offsets and one-based line/column positions for resolved references.

## Current scope

- Case-insensitive Delphi identifiers and dotted unit names.
- Interface and implementation `uses` clauses.
- Exported interface types and routines.
- Unqualified reference resolution against the explicitly visible units.
- Exact half-open byte ranges over the original source, including CRLF input.
- Comments and string literals excluded by the lexer.
- Conservative protection for units containing `initialization` or `finalization`.
- Human-readable and JSON reports.

## Known limitations

This first vertical slice is not a complete Delphi compiler frontend. Conditional compilation, overload resolution, class/member lookup, generics, helpers, `with`, include files, project unit scopes, package metadata, encoding-aware columns and full local lexical scopes are not implemented yet. Unsupported or ambiguous ownership must remain `UNKNOWN`; callers should never convert it to `REMOVE`.

The AST and report preserve source ranges so future transformations can generate minimal edits rather than reformatting Delphi files.
