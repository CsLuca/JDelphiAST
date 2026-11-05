#pragma once

#include "jdelphiast/analyzer.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace jdelphiast {

struct PreprocessorOptions {
  std::vector<std::string> defines;
  std::vector<std::filesystem::path> includePaths;
  std::size_t maxIncludeDepth{64};
  std::optional<double> compilerVersion;
};

struct PreprocessResult {
  struct SourceOrigin {
    std::filesystem::path file;
    SourcePosition position;
  };
  struct IncludedSource {
    std::filesystem::path file;
    std::string source;
  };
  std::string source;
  std::string expandedSource;
  // One entry per expanded byte plus EOF; original source remains untouched.
  std::vector<SourceOrigin> origins;
  std::vector<IncludedSource> includedSources;
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
