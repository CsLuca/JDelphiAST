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
  MainUnit in 'src\MainUnit.pas',
  SharedUnit in 'shared\SharedUnit.pas';
end.)");
  write(root / "Demo.dproj", R"(<Project><PropertyGroup>
<Base>True</Base>
<DCC_UnitSearchPath>$(PROJECTDIR)\shared</DCC_UnitSearchPath>
<DCC_IncludePath>$(PROJECTDIR)\include</DCC_IncludePath>
<DCC_Namespace>System;Vcl</DCC_Namespace>
<DCC_Define>PLUGIN</DCC_Define>
</PropertyGroup>
<PropertyGroup Condition="'$(Config)'=='Base' or '$(Base)'!=''"><Base>true</Base></PropertyGroup>
<PropertyGroup Condition="('$(Platform)'=='Win32' and '$(Base)'=='true') or '$(Base_Win32)'!=''"><Base_Win32>true</Base_Win32><Base>true</Base></PropertyGroup>
<PropertyGroup Condition="'$(Config)'=='Debug' or '$(Cfg_1)'!=''"><Cfg_1>true</Cfg_1><DCC_Define>DEBUG;$(DCC_Define)</DCC_Define></PropertyGroup>
<PropertyGroup Condition="'$(Config)'=='Release' or '$(Cfg_2)'!=''"><Cfg_2>true</Cfg_2><DCC_Define>RELEASE;$(DCC_Define)</DCC_Define></PropertyGroup>
<PropertyGroup Condition="('$(Platform)'=='Win32' and '$(Cfg_2)'=='true') or '$(Cfg_2_Win32)'!=''"><Cfg_2_Win32>true</Cfg_2_Win32><DCC_Define>WIN32;$(DCC_Define)</DCC_Define></PropertyGroup>
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
function MakeValue(E: Exception; const S: string; B: Boolean): string;
implementation
procedure UseShared; begin end;
function MakeValue(E: Exception; const S: string; B: Boolean): string; begin Result := S; end;
end.)");
  write(root / "rtl.jdi", "System.SysUtils|Exception|initialization,finalization\n");

  jdelphiast::ProjectOptions options;
  options.indexFiles.push_back(root / "rtl.jdi");
  options.unitMappingCatalog = root / "uses-map.json";
  write(root / "uses-map.json", "{\"schema_version\":\"1.0\",\"mappings\":[]}");
  const auto loaded = jdelphiast::loadPackage(root / "Demo.dpk", options);
  require(loaded.diagnostics.empty(), "realistic Release Win32 DPROJ conditions are fully evaluated");
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
  require(json.find("\"schemaVersion\":1,\"schema_version\":\"2.1\"") != std::string::npos,
          "project JSON preserves legacy schema marker and adds v2 marker");
  require(json.find("\"sourceHash\":\"sha256:") != std::string::npos, "project JSON contains SHA-256");
  require(json.find("\"uses\":{\"interface\":[") != std::string::npos, "project JSON groups uses");
  require(json.find("\"declarations\":[") != std::string::npos, "project JSON contains declarations");
  require(json.find("\"calls\":[") != std::string::npos, "project JSON contains calls");
  require(json.find("\"assignments\":[") != std::string::npos, "project JSON contains assignments");
  require(json.find("\"reasons\":[") != std::string::npos, "dependency reasons are serialized");
  require(json.find("\"configuration\":\"Release\",\"platform\":\"Win32\"") != std::string::npos,
          "selected build context is serialized");
  require(json.find("\"preprocessor\":{\"complete\":true") != std::string::npos,
          "preprocessor completeness is serialized");
  require(json.find("\"recommendations\":{\"safeRemoveUses\":[") != std::string::npos,
          "automation recommendations are serialized");
  require(json.find("\"unit_mapping\":{\"available\":true") != std::string::npos &&
              json.find("uses-map.json") != std::string::npos,
          "optional unit mapping catalog reference is serialized additively");
  require(json.find("\"blockedRemovals\":[") != std::string::npos,
          "blocked removals are serialized");
  require(json.find("\"uses\":[") != std::string::npos && json.find("\"references\":[") != std::string::npos &&
              json.find("\"symbols\":[") != std::string::npos && json.find("\"inheritance\":[") != std::string::npos &&
              json.find("\"dependencies\":[") != std::string::npos && json.find("\"diagnostics\":[") != std::string::npos,
          "v2 additive report sections are serialized");

  const auto generatedIndex = jdelphiast::createSymbolIndex({root}, "v600");
  require(generatedIndex.find("SharedUnit|") != std::string::npos, "automatic index contains unit");
  require(generatedIndex.find("MakeValue") != std::string::npos, "automatic index contains exported symbol");
  require(generatedIndex.find(",S,") == std::string::npos && generatedIndex.find(",Enabled|") == std::string::npos,
          "routine parameters are not exported as unit symbols");
  write(root / "generated.jdi", generatedIndex);
  const auto roundTrip = jdelphiast::loadSymbolIndex(root / "generated.jdi");
  require(!roundTrip.empty(), "index declarations round trip");
  bool foundStringReturn = false;
  bool foundConstModifier = false;
  for (const auto& indexedUnit : roundTrip)
    for (const auto& declaration : indexedUnit.declarations)
      if (declaration.name == "MakeValue" && declaration.type == "string") {
        foundStringReturn = true;
        foundConstModifier = declaration.parameters.size() >= 2 && declaration.parameters[1].modifier == "const";
      }
  require(foundStringReturn, "index preserves function return type");
  require(foundConstModifier, "index preserves parameter modifiers");
  const auto mainIndexed = std::find_if(roundTrip.begin(), roundTrip.end(), [](const auto& item) {
    return item.name == "SharedUnit";
  });
  require(mainIndexed != roundTrip.end() && mainIndexed->packageName == "Demo" &&
              mainIndexed->packageDcp == "Demo.dcp" && mainIndexed->packageBpl == "Demo.bpl",
          "index derives package metadata from an actual DPK");
  const auto bundled = jdelphiast::bundledSymbolIndex();
  const auto hasBundledUnit = [&](const std::string& name) {
    return std::any_of(bundled.begin(), bundled.end(), [&](const auto& item) { return item.name == name; });
  };
  require(hasBundledUnit("System.SysUtils") && hasBundledUnit("Winapi.Windows") &&
              hasBundledUnit("CSCore.Types") && hasBundledUnit("CSCore.Note.Utils"),
          "embedded index provides RTL WinAPI and V600 units without external files");
  const auto noteUnit = std::find_if(bundled.begin(), bundled.end(), [](const auto& item) {
    return item.name == "CSCore.Note.Utils";
  });
  require(noteUnit != bundled.end() && !noteUnit->declarations.empty() &&
              noteUnit->declarations.front().type == "Integer",
          "embedded V600 declarations preserve return types");

  std::filesystem::remove_all(root);
  std::cout << "All project tests passed\n";
}
