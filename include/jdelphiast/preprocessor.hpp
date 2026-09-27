#pragma once

#include "jdelphiast/analyzer.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace jdelphiast {

struct PreprocessorOptions {
  std::vector<std::string> defines;
  std::vector<std::filesystem::path> includePaths;
  std::size_t maxIncludeDepth{64};
};

struct PreprocessResult {
  std::string source;
  std::vector<std::filesystem::path> includesResolved;
  std::vector<InactiveRange> inactiveRanges;
  std::vector<std::string> reasons;
  std::vector<std::string> finalDefines;
  bool complete{true};
};

[[nodiscard]] PreprocessResult preprocess(const std::filesystem::path& file,
                                          std::string_view source,
                                          const PreprocessorOptions& options = {});

}  // namespace jdelphiast
