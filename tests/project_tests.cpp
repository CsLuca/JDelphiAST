#include "jdelphiast/project.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

void write(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << content;
}

const jdelphiast::UnitAnalysis& unit(const jdelphiast::AnalysisResult& result, const std::string& name) {
  for (const auto& candidate : result.units)
    if (candidate.ast.name == name) return candidate;
  std::cerr << "FAILED: missing unit " << name << '\n';
  std::exit(1);
}

const jdelphiast::Dependency& dependency(const jdelphiast::UnitAnalysis& value, const std::string& name) {
  for (const auto& candidate : value.dependencies)
    if (candidate.unit == name) return candidate;
  std::cerr << "FAILED: missing dependency " << name << '\n';
  std::exit(1);
}

}  // namespace

int main() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() / ("jdelphiast-project-" + std::to_string(stamp));
  const auto sourceDir = root / "src";
  const auto sharedDir = root / "shared";

  write(root / "Demo.dpk", R"(package Demo;
requires rtl;
contains
  MainUnit in 'src\MainUnit.pas';
end.)");
  write(root / "Demo.dproj", R"(<Project><PropertyGroup>
<DCC_UnitSearchPath>$(PROJECTDIR)\shared</DCC_UnitSearchPath>
<DCC_IncludePath>$(PROJECTDIR)\include</DCC_IncludePath>
<DCC_Namespace>System;Vcl</DCC_Namespace>
<DCC_Define>PLUGIN</DCC_Define>
</PropertyGroup>
<PropertyGroup Condition="'$(Config)'=='Debug'"><DCC_Define>DEBUG;$(DCC_Define)</DCC_Define></PropertyGroup>
<PropertyGroup Condition="'$(Config)'=='Release' and '$(Platform)'=='Win32'"><DCC_Define>RELEASE;WIN32;$(DCC_Define)</DCC_Define></PropertyGroup>
</Project>)");
  write(sourceDir / "MainUnit.pas", R"(unit MainUnit;
interface
{$IFDEF DEBUG}
uses MissingDebugUnit;
{$ELSE}
uses SysUtils, SharedUnit;
{$ENDIF}
procedure Run(E: Exception);
implementation
procedure Run(E: Exception);
var Worker: TWorker;
begin
  Worker := TWorker.Create(nil);
  Worker.Value := MakeValue(E, '', True);
end;
end.)");
  write(sharedDir / "SharedUnit.pas", R"(unit SharedUnit;
interface
procedure UseShared;
type TWorker = class end;
function MakeValue(E: Exception; S: string; B: Boolean): string;
implementation
procedure UseShared; begin end;
function MakeValue(E: Exception; S: string; B: Boolean): string; begin Result := S; end;
end.)");
  write(root / "rtl.jdi", "System.SysUtils|Exception|initialization,finalization\n");

  jdelphiast::ProjectOptions options;
  options.indexFiles.push_back(root / "rtl.jdi");
  const auto loaded = jdelphiast::loadPackage(root / "Demo.dpk", options);
  require(loaded.sourceFiles.size() == 2, "DPK and DPROJ discover package sources recursively");
  require(loaded.options.defines.size() == 5, "Release Win32 and predefined compiler symbols are loaded");
  require(std::find(loaded.options.defines.begin(), loaded.options.defines.end(), "DEBUG") == loaded.options.defines.end(),
          "inactive Debug DPROJ properties are excluded");
  require(std::find(loaded.options.defines.begin(), loaded.options.defines.end(), "MSWINDOWS") != loaded.options.defines.end(),
          "Win32 predefined compiler symbols are supplied");
  require(loaded.options.includePaths.size() == 1, "DPROJ include path is loaded");
  require(loaded.options.namespaces.size() == 2, "DPROJ namespaces are loaded");
  const auto result = loaded.analyzer.analyze();
  const auto& mainUnit = unit(result, "MainUnit");
  require(dependency(mainUnit, "SysUtils").status == jdelphiast::DependencyStatus::Used,
          "DPROJ namespace alias resolves RTL symbol from pre-generated index");
  require(dependency(mainUnit, "SharedUnit").status == jdelphiast::DependencyStatus::Used,
          "search path resolves and loads local unit recursively");
  require(std::none_of(mainUnit.dependencies.begin(), mainUnit.dependencies.end(), [](const auto& item) {
            return item.unit == "MissingDebugUnit";
          }), "inactive DEBUG uses is excluded in Release configuration");
  require(!mainUnit.ast.calls.empty(), "calls are extracted from package units");
  require(mainUnit.ast.assignments.size() == 2, "assignments are extracted from package units");
  const auto json = jdelphiast::toProjectJson(result, "Demo", root, "2026-09-25T22:30:00");
  require(json.find("\"plugin\":\"Demo\"") != std::string::npos, "project JSON contains plugin");
  require(json.find("\"sourceHash\":\"sha256:") != std::string::npos, "project JSON contains SHA-256");
  require(json.find("\"uses\":{\"interface\":[") != std::string::npos, "project JSON groups uses");
  require(json.find("\"declarations\":[") != std::string::npos, "project JSON contains declarations");
  require(json.find("\"calls\":[") != std::string::npos, "project JSON contains calls");
  require(json.find("\"assignments\":[") != std::string::npos, "project JSON contains assignments");
  require(json.find("\"reasons\":[") != std::string::npos, "dependency reasons are serialized");
  require(json.find("\"preprocessor\":{\"configuration\":\"Release\",\"platform\":\"Win32\"") != std::string::npos,
          "selected build context is serialized");

  const auto generatedIndex = jdelphiast::createSymbolIndex({sharedDir});
  require(generatedIndex.find("SharedUnit|") != std::string::npos, "automatic index contains unit");
  require(generatedIndex.find("MakeValue") != std::string::npos, "automatic index contains exported symbol");
  require(generatedIndex.find(",S,") == std::string::npos && generatedIndex.find(",Enabled|") == std::string::npos,
          "routine parameters are not exported as unit symbols");
  write(root / "generated.jdi", generatedIndex);
  const auto roundTrip = jdelphiast::loadSymbolIndex(root / "generated.jdi");
  require(!roundTrip.empty() && !roundTrip.front().declarations.empty(), "index declarations round trip");
  bool foundStringReturn = false;
  for (const auto& declaration : roundTrip.front().declarations)
    if (declaration.name == "MakeValue" && declaration.type == "string") foundStringReturn = true;
  require(foundStringReturn, "index preserves function return type");

  std::filesystem::remove_all(root);
  std::cout << "All project tests passed\n";
}
