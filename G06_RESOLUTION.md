# G06 — scoped symbol resolution

Query expression: explicit scope/import/receiver and exact overload checks,
var/out lvalues, nested scope fixture, class field/property chain and ancestor
method. Unknown casts/with/Variant/defaults remain unresolved.
Diagnostic lookup hardened against foreign metadata and callable-as-variable.
JDI cache fingerprint covers every byte; unchanged size/timestamp content edit
invalidates sidecar, verified. Source contexts reread rather than cached.

47 C++/CLI tests, 337 converter tests, GUI pilot 7 BUILD_OK/0 errors.
Reports locally quality-conversion-fllifranchini/runs/g06-01.
TWFTes hierarchy override statuses remain unknown; never authorize a patch
from those unknown entries. Scope is the tested fixture/pilot constructs,
not a full Delphi compiler semantic resolver. Official executables unchanged.
No commit/push performed.
