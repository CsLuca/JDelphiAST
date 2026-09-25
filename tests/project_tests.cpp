#include "jdelphiast/project.hpp"

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
<DCC_Namespace>System;Vcl</DCC_Namespace>
<DCC_Define>DEBUG;PLUGIN</DCC_Define>
</PropertyGroup></Project>)");
  write(sourceDir / "MainUnit.pas", R"(unit MainUnit;
interface
uses SysUtils, SharedUnit;
procedure Run(E: Exception);
implementation
procedure Run(E: Exception); begin UseShared; end;
end.)");
  write(sharedDir / "SharedUnit.pas", R"(unit SharedUnit;
interface
procedure UseShared;
implementation
procedure UseShared; begin end;
end.)");
  write(root / "rtl.jdi", "System.SysUtils|Exception|initialization,finalization\n");

  jdelphiast::ProjectOptions options;
  options.indexFiles.push_back(root / "rtl.jdi");
  const auto loaded = jdelphiast::loadPackage(root / "Demo.dpk", options);
  require(loaded.sourceFiles.size() == 2, "DPK and DPROJ discover package sources recursively");
  require(loaded.options.defines.size() == 2, "DPROJ defines are loaded");
  require(loaded.options.namespaces.size() == 2, "DPROJ namespaces are loaded");
  const auto result = loaded.analyzer.analyze();
  const auto& mainUnit = unit(result, "MainUnit");
  require(dependency(mainUnit, "SysUtils").status == jdelphiast::DependencyStatus::Used,
          "DPROJ namespace alias resolves RTL symbol from pre-generated index");
  require(dependency(mainUnit, "SharedUnit").status == jdelphiast::DependencyStatus::Used,
          "search path resolves and loads local unit recursively");

  std::filesystem::remove_all(root);
  std::cout << "All project tests passed\n";
}
