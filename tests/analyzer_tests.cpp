#include "jdelphiast/analyzer.hpp"

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
  RegisterHandlers;
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
  require(dependency(consumer, "Registration").status == jdelphiast::DependencyStatus::SideEffect,
          "initialization unit is protected");
  require(dependency(consumer, "Unused").status == jdelphiast::DependencyStatus::Unused,
          "known empty unit is unused");
  require(dependency(consumer, "Vendor.Secret").status == jdelphiast::DependencyStatus::Unknown,
          "missing source is unknown");
  require(dependency(consumer, "System.SysUtils").references.front().range.begin.line == 10,
          "CRLF line is preserved");
  require(jdelphiast::toJson(result).find("\"schemaVersion\":1") != std::string::npos,
          "JSON report has schema version");
  require(result.graph.size() == consumer.dependencies.size(), "dependency graph contains uses edges");

  jdelphiast::Analyzer noise;
  noise.addSource("Known.pas", "unit Known; interface type Exception = class end; implementation end.");
  noise.addSource("Noise.pas", "unit Noise; interface uses Known; implementation begin {Exception} Writeln('Exception'); end.");
  const auto noiseResult = noise.analyze();
  require(dependency(noiseResult.units[1], "Known").status == jdelphiast::DependencyStatus::Unused,
          "comments and strings do not create references");

  const auto incomplete = jdelphiast::parseUnit("Incomplete.pas", "unit Incomplete; interface procedure P(A:");
  require(!incomplete.complete, "truncated routine declaration is incomplete without crashing");

  std::cout << "All tests passed\n";
}
