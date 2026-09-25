#pragma once

#include "jdelphiast/analyzer.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace jdelphiast {

struct ProjectOptions {
  std::vector<std::filesystem::path> searchPaths;
  std::vector<std::string> namespaces;
  std::vector<std::string> defines;
  std::vector<std::filesystem::path> indexFiles;
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

}  // namespace jdelphiast
