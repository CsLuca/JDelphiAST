#pragma once

#include "jdelphiast/analyzer.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace jdelphiast {

struct ProjectOptions {
  std::vector<std::filesystem::path> searchPaths;
  std::vector<std::filesystem::path> includePaths;
  std::vector<std::string> namespaces;
  std::vector<std::string> defines;
  std::vector<std::filesystem::path> indexFiles;
  std::string configuration{"Release"};
  std::string platform{"Win32"};
  std::filesystem::path dprojFile;
};

struct ProjectLoadResult {
  Analyzer analyzer;
  ProjectOptions options;
  std::vector<std::filesystem::path> sourceFiles;
  std::vector<std::string> diagnostics;
};

[[nodiscard]] ProjectLoadResult loadPackage(const std::filesystem::path& packageFile,
                                            ProjectOptions options = {});
[[nodiscard]] std::vector<IndexedUnit> loadSymbolIndex(const std::filesystem::path& indexFile);
[[nodiscard]] std::vector<IndexedUnit> bundledSymbolIndex();
[[nodiscard]] std::string createSymbolIndex(const std::vector<std::filesystem::path>& sources);

}  // namespace jdelphiast
