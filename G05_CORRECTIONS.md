# G05 correction campaign — scoped qualification passed

Final verification: 46 C++/CLI tests, actual concrete token roundtrip on
41 pilot PAS, 337 converter tests, GUI candidate 7 BUILD_OK/0 errors.
Include source-order expansion with original-file byte map, anonymous method
nodes and UTF8/byte-envelope JSON implemented. Final reports runs/g05-02.
Full grammar validation/Unicode identifiers/advanced generic and anonymous
type semantics remain unresolved; cross-file patch spans require review.
Official executables unchanged, no commit/push. Earlier notes below record
intermediate limitations, superseded where the final verification says so.

Base: 6622c01. Build: CMake/Ninja, MSYS2 UCRT64 GCC16.2, C++20 Release.

Implemented and checked:
- optional indexes copy allows building a source checkout without local indexes;
- UTF8 BOM ignored as syntax but original byte ranges retained;
- type headers independent of const parameter modifiers;
- generic type headers and members; record-helper kind/target;
- member declarations and overload exports retain distinct signatures;
- method directive scanning preserves override diagnostics;
- property tokens no longer duplicated as fields;
- anonymous `begin` no longer invented as a named procedure;
- malformed delimiter partial parse returns diagnostic rather than null-string exception;
- incomplete query reports provisional exports;
- byte-preserving concrete token envelope and writer, including trivia/BOM/CRLF;
- include code candidate declarations carry source_file and original include ranges;
  full source-order expansion remains explicitly incomplete.

46 C++/CLI tests pass (including dedicated qualification test); actual concrete
token roundtrip and declaration-range bounds pass on 41 pilot Pascal files.
Reports: %TEMP%/opencode/quality-conversion-fllifranchini/runs/g05-02.

Not yet a G05 PASS:
- complete source-order include expansion/semantic provenance remains pending;
- anonymous method represented only as tokens/calls, not a complete routine node;
- ANSI source bytes in project JSON need an explicit encoding/UTF8 output contract;
- parser cannot yet provide full Delphi syntax validation from structural scans.

Do not replace the official parser or converter executable with this candidate
until qualification is completed. No commit or push has been made.
