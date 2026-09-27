#include "jdelphiast/analyzer.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

const jdelphiast::Dependency& dependency(const jdelphiast::UnitAnalysis& unit, const std::string& name) {
  for (const auto& item : unit.dependencies)
    if (item.unit == name) return item;
  std::cerr << "FAILED: missing dependency " << name << '\n';
  std::exit(1);
}

}  // namespace

int main() {
  jdelphiast::Analyzer analyzer;
  analyzer.addSource("System.SysUtils.pas", R"(unit System.SysUtils;
interface
type Exception = class end;
implementation
end.)");
  analyzer.addSource("System.Classes.pas", R"(unit System.Classes;
interface
type TStringList = class end;
implementation
end.)");
  analyzer.addSource("Registration.pas", R"(unit Registration;
interface
implementation
initialization
  RegisterClass(TRegisteredHandler);
finalization
  UnregisterHandlers;
end.)");
  analyzer.addSource("Unused.pas", R"(unit Unused;
interface
implementation
end.)");
  analyzer.addSource("Consumer.pas", "unit Consumer;\r\ninterface\r\nuses System.SysUtils, System.Classes, Vendor.Secret;\r\nimplementation\r\nuses Registration, Unused;\r\nprocedure Run;\r\nvar Items: TStringList;\r\nbegin\r\n// Exception TStringList\r\ntry raise Exception.Create('System.Classes') except end;\r\nend;\r\nend.\r\n");

  const auto result = analyzer.analyze();
  require(result.units.size() == 5, "all units parsed");
  const auto& consumer = result.units[4];
  require(dependency(consumer, "System.SysUtils").status == jdelphiast::DependencyStatus::Used,
          "unqualified Exception resolves to System.SysUtils");
  require(dependency(consumer, "System.Classes").status == jdelphiast::DependencyStatus::Used,
          "unqualified TStringList resolves to System.Classes");
  require(dependency(consumer, "Registration").status == jdelphiast::DependencyStatus::Used,
          "initialization unit is protected as used");
  require(!dependency(consumer, "Registration").reasons.empty(), "side effect reasons are reported");
  require(std::find(dependency(consumer, "Registration").reasons.begin(),
                    dependency(consumer, "Registration").reasons.end(),
                    "class_registration: TRegisteredHandler") != dependency(consumer, "Registration").reasons.end(),
          "class registration reason is reported");
  require(dependency(consumer, "Unused").status == jdelphiast::DependencyStatus::Unused,
          "known empty unit is unused");
  require(dependency(consumer, "Unused").reasons.size() == 1 &&
              dependency(consumer, "Unused").reasons.front() == "no_references",
          "unused dependency has only no_references reason");
  require(dependency(consumer, "Vendor.Secret").status == jdelphiast::DependencyStatus::Unknown,
          "missing source is unknown");
  require(dependency(consumer, "System.SysUtils").references.front().range.begin.line == 10,
          "CRLF line is preserved");
  require(jdelphiast::toJson(result).find("\"schemaVersion\":1") != std::string::npos,
          "JSON report has schema version");
  require(jdelphiast::toJson(result).find("source_unit_not_indexed: Vendor.Secret") != std::string::npos,
          "direct JSON contains dependency reasons");
  require(result.graph.size() == consumer.dependencies.size(), "dependency graph contains uses edges");

  jdelphiast::Analyzer noise;
  noise.addSource("Known.pas", "unit Known; interface type Exception = class end; implementation end.");
  noise.addSource("Noise.pas", "unit Noise; interface uses Known; implementation begin {Exception} Writeln('Exception'); end.");
  const auto noiseResult = noise.analyze();
  require(dependency(noiseResult.units[1], "Known").status == jdelphiast::DependencyStatus::Unused,
          "comments and strings do not create references");

  jdelphiast::Analyzer unresolved;
  unresolved.addIndexedUnit({"Known", {}, {}, false, false, false});
  unresolved.addSource("UsesUnknown.pas", "unit UsesUnknown; interface uses Known; procedure P; implementation procedure P; begin MissingCall; end; end.");
  const auto unresolvedResult = unresolved.analyze();
  require(dependency(unresolvedResult.units[0], "Known").status == jdelphiast::DependencyStatus::Unknown,
          "unresolved symbols prevent unsafe unused classification");

  const auto incomplete = jdelphiast::parseUnit("Incomplete.pas", "unit Incomplete; interface procedure P(A:");
  require(!incomplete.complete, "truncated routine declaration is incomplete without crashing");

  jdelphiast::Analyzer scoped;
  scoped.addSource("ExternalUnit.pas", "unit ExternalUnit; interface procedure Open; implementation end.");
  scoped.addSource("Scoped.pas", R"(unit Scoped;
interface uses ExternalUnit;
implementation
procedure A;
var Open: Boolean;
begin Open := True; end;
procedure B;
begin Open; end;
end.)");
  const auto scopedResult = scoped.analyze();
  require(dependency(scopedResult.units[1], "ExternalUnit").status == jdelphiast::DependencyStatus::Used,
          "a local in one routine does not hide an imported symbol in another routine");

  std::cout << "All tests passed\n";
}
