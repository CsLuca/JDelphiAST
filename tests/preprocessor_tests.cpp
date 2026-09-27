#include "jdelphiast/preprocessor.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
  if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void write(const std::filesystem::path& path, const std::string& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary); output << value;
}
}

int main() {
  const auto root = std::filesystem::temp_directory_path() /
      ("jdelphiast-preprocessor-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  write(root / "defines.inc", "{$DEFINE FROM_INCLUDE}\n");
  const std::string source = R"(unit U;
interface
{$IFDEF DEBUG}
uses DebugUnit;
{$ELSE}
uses ReleaseUnit;
{$ENDIF}
{$I defines.inc}
{$IF DEFINED(FROM_INCLUDE)}
const Enabled = True;
{$ENDIF}
implementation
end.)";
  jdelphiast::PreprocessorOptions options;
  options.defines = {"RELEASE"};
  options.includePaths = {root};
  const auto result = jdelphiast::preprocess(root / "U.pas", source, options);
  require(result.source.size() == source.size(), "preprocessing preserves byte offsets");
  require(result.source.find("DebugUnit") == std::string::npos, "inactive IFDEF branch is masked");
  require(result.source.find("ReleaseUnit") != std::string::npos, "active ELSE branch remains");
  require(result.source.find("Enabled") != std::string::npos, "include DEFINE affects following source");
  require(result.includesResolved.size() == 1, "include is resolved");
  require(!result.inactiveRanges.empty(), "inactive ranges are reported");
  std::filesystem::remove_all(root);
  std::cout << "All preprocessor tests passed\n";
}
