# JDelphiAST

[![CI](https://github.com/CsLuca/JDelphiAST/actions/workflows/ci.yml/badge.svg)](https://github.com/CsLuca/JDelphiAST/actions/workflows/ci.yml)

JDelphiAST is an experimental C++20 static analyzer for Delphi/Object Pascal. It parses Delphi units, builds a source-oriented AST, indexes exported symbols, resolves references through visible `uses` clauses, and produces a dependency graph.

The first supported analysis classifies each declared dependency as:

| Status | Meaning | Recommendation |
| --- | --- | --- |
| `USED` | At least one symbol resolves to the unit | Keep |
| `UNUSED` | The complete indexed unit has no resolved references or detected side effects | Remove candidate |
| `SIDE-EFFECT` | The unit has initialization or finalization code | Keep |
| `UNKNOWN` | Source, syntax, or symbol ownership is incomplete or ambiguous | Review |

JDelphiAST is deliberately conservative. It recommends removal only for `UNUSED` dependencies with `HIGH` confidence. Missing source, duplicate units, unsupported directives, incomplete syntax, and ambiguous symbols never become automatic removal recommendations.

## Why AST Analysis

A textual search for `System.SysUtils.` cannot detect normal unqualified Delphi references:

```pascal
uses
  System.SysUtils;

try
  Execute;
except
  on E: Exception do
    Handle(E);
end;
```

`Exception` belongs to `System.SysUtils`, even though the fully qualified name does not occur in the source. JDelphiAST indexes declarations and resolves visible identifiers instead of searching for unit-name strings.

It also protects units imported only for startup behavior:

```pascal
unit MyPlugin.Register;

interface
implementation

initialization
  RegisterMyComponent;

end.
```

Such a dependency is reported as `SIDE-EFFECT`, not `UNUSED`.

## Architecture

```text
Delphi source
     |
     v
Lexer and source ranges
     |
     v
Unit AST ---------------- Uses clauses
     |                         |
     v                         |
Exported symbol index          |
     |                         |
     +----------+--------------+
                |
                v
       Conservative resolver
                |
                v
         Dependency graph
                |
       +--------+---------+-------------+
       |        |         |             |
     USED    UNUSED   SIDE-EFFECT    UNKNOWN
```

The graph records the source unit, target unit, `interface` or `implementation` section, classification, reference evidence, and confidence. Cycles are exposed in the JSON result.

## Requirements

- CMake 3.20 or newer
- A C++20 compiler
- Ninja or another CMake-supported build tool

The Windows build has been verified with:

```text
MSYS2 UCRT64
GCC 16.2.0
CMake 4.4.2
Ninja 1.13.2
```

## Build With MSYS2 UCRT64

Install the required packages from an MSYS2 UCRT64 terminal:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja
```

From the repository directory in the **MSYS2 UCRT64** terminal:

```sh
cmake -S . -B build-ucrt64 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ucrt64
ctest --test-dir build-ucrt64 --output-on-failure
```

From Windows PowerShell, with MSYS2 installed in `C:\msys64`:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"

cmake -S . -B build-ucrt64 -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=C:\msys64\ucrt64\bin\g++.exe

cmake --build build-ucrt64
ctest --test-dir build-ucrt64 --output-on-failure
```

Generated programs:

```text
build-ucrt64/jdelphiast_cli.exe
build-ucrt64/jdelphiast_tests.exe
```

Keep `C:\msys64\ucrt64\bin` in `PATH` when running binaries linked against the MSYS2 UCRT64 runtime.

## Generic Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Tests can be disabled for embedding builds:

```sh
cmake -S . -B build -DJDELPHIAST_BUILD_TESTS=OFF
```

## CLI Usage

Pass the unit to analyze together with all available source units needed to build the symbol index:

```powershell
build-ucrt64\jdelphiast_cli.exe `
  MyPlugin.pas `
  System.SysUtils.pas `
  System.Classes.pas `
  Vcl.Forms.pas `
  MyPlugin.Core.pas `
  MyPlugin.Database.pas
```

Use `--json` for machine-readable output:

```powershell
build-ucrt64\jdelphiast_cli.exe --json MyPlugin.pas System.SysUtils.pas
```

Example text report:

```text
MyPlugin.pas
USES ANALYSIS
System.SysUtils       USED          4 references
System.Classes        USED          2 references
Vcl.Forms             USED          1 references
MyPlugin.Core         USED          7 references
MyPlugin.Database     UNUSED        0 references
MyPlugin.Register     SIDE-EFFECT   0 references
Vendor.Secret         UNKNOWN       0 references

Recommendation:
  REMOVE MyPlugin.Database
```

The JSON report has a versioned top-level schema:

```json
{
  "schemaVersion": 1,
  "units": [],
  "graph": [],
  "cycles": []
}
```

Resolved references include half-open byte offsets and one-based line and column positions. Offsets refer to the original input bytes, including CRLF line endings, so future transformations can create minimal patches without reformatting the Delphi source.

## Library API

```cpp
#include <jdelphiast/analyzer.hpp>

#include <iostream>

int main() {
  jdelphiast::Analyzer analyzer;
  analyzer.addSource("System.SysUtils.pas", systemSysUtilsSource);
  analyzer.addSource("MyPlugin.pas", myPluginSource);

  const auto result = analyzer.analyze();
  std::cout << jdelphiast::toJson(result) << '\n';
}
```

Primary public data structures are declared in `include/jdelphiast/analyzer.hpp`:

- `UnitAst`
- `UsesItem`
- `SymbolDeclaration`
- `SymbolReference`
- `Dependency`
- `DependencyEdge`
- `UnitAnalysis`
- `AnalysisResult`
- `Analyzer`

## Current Scope

- Case-insensitive Delphi identifiers.
- Namespaced and dotted unit names.
- `interface` and `implementation` uses clauses.
- Exported interface types, constants, variables, and routines.
- Qualified and unqualified symbol references.
- Visibility checks between interface and implementation sections.
- Comments and string literals excluded at lexer level.
- Exact source ranges over the original input.
- Explicit `initialization` and `finalization` detection.
- Conservative detection of implicit unit initialization blocks.
- Duplicate-unit and missing-source protection.
- Text and JSON reports.
- Dependency graph and cycle reporting.

## Safety Rules

- A missing target source is `UNKNOWN`, never `UNUSED`.
- An incomplete source is not considered safe to remove.
- An unsupported compiler directive lowers analysis certainty.
- An ambiguous reference is never assigned to the first matching unit.
- An implementation-only unit cannot satisfy an interface reference.
- Comments and strings never create symbol references.
- Initialization and finalization protect a dependency even when no exported symbol is referenced.

## Known Limitations

This release is a vertical slice, not a complete Delphi compiler frontend. It does not yet implement:

- full lexical and class member scopes;
- overload and callable-signature resolution;
- inherited member lookup;
- complete generics, helpers, anonymous methods, and attributes;
- semantic handling of `with`;
- evaluation of conditional compilation branches;
- include-file expansion;
- project-specific unit scope names;
- package, DCU, DCP, and BPL metadata;
- source encoding-aware Unicode columns;
- persisted/versioned V600 reference indexes;
- automatic source patch generation.

Unsupported or ambiguous cases must remain `UNKNOWN`. Consumers must not reinterpret them as `REMOVE`.

## Roadmap

- Complete Delphi expression and declaration AST.
- Lexical scopes and type-aware member resolution.
- Configurable compiler defines and include paths.
- Persisted AST/symbol indexes for Delphi reference libraries.
- Public API and impact analysis.
- Strongly connected component reports for circular dependencies.
- Minimal, idempotent edits for safe `uses` cleanup.
- DOT export for dependency visualization.

## License

No license has been selected yet. Until a license file is added, normal copyright restrictions apply.
