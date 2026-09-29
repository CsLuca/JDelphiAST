#pragma once

#include "jdelphiast/analyzer.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
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
  std::filesystem::path unitMappingCatalog;
};

struct ProjectLoadResult {
  Analyzer analyzer;
  ProjectOptions options;
  std::vector<std::filesystem::path> sourceFiles;
  std::vector<std::string> diagnostics;
};

struct IndexDiagnostic {
  std::filesystem::path sourceFile;
  std::string indexStatus;
  std::string code;
  std::string severity;
  std::string message;
};

struct ValidationDiagnostic {
  std::string code;
  std::string severity;
  std::string message;
  std::filesystem::path file;
  std::string unit;
};

struct ValidationPolicy {
  std::unordered_map<std::string, std::string> severities;
};

struct QueryPerformance {
  long long indexLoadMs{};
  long long indexLookupMs{};
  long long sourceParseMs{};
  long long sourceScanMs{};
  long long packageEnrichmentMs{};
  long long legacyExportLookupMs{};
  long long mappingLookupMs{};
  long long v500TypeLookupMs{};
  long long v600TypeLookupMs{};
  long long targetMemberAnalysisMs{};
  long long totalMs{};
  std::size_t timeoutMs{10000};
  bool cacheHit{};
};

struct IndexStatistics {
  std::size_t filesScanned{};
  std::size_t unitsIndexed{};
  std::size_t unitsPartiallyIndexed{};
  std::size_t filesSkippedNonUnitSource{};
  std::size_t filesParseFailed{};
  std::size_t exportsIndexed{};
};

struct IndexBuildOptions {
  std::string version{"unknown"};
  std::string origin;
  std::vector<std::filesystem::path> sourceRoots;
  std::vector<std::filesystem::path> packageRoots;
  std::filesystem::path validationPolicy;
};

struct IndexBuildResult {
  std::string content;
  IndexStatistics statistics;
  std::vector<IndexDiagnostic> diagnostics;
  bool hasBlockingErrors{};
};

struct IndexLoadResult {
  std::vector<IndexedUnit> units;
  std::vector<ValidationDiagnostic> diagnostics;
  bool hasBlockingErrors{};
};

[[nodiscard]] ProjectLoadResult loadPackage(const std::filesystem::path& packageFile,
                                            ProjectOptions options = {});
[[nodiscard]] std::vector<IndexedUnit> loadSymbolIndex(const std::filesystem::path& indexFile);
[[nodiscard]] std::vector<IndexedUnit> loadSymbolIndexCached(const std::filesystem::path& indexFile,
                                                              bool* cacheHit = nullptr);
[[nodiscard]] std::shared_ptr<const std::vector<IndexedUnit>> loadSymbolIndexShared(
    const std::filesystem::path& indexFile, bool* cacheHit = nullptr);
[[nodiscard]] std::vector<IndexedUnit> loadSymbolIndexFiltered(
    const std::filesystem::path& indexFile, const std::vector<std::string>& terms);
[[nodiscard]] IndexLoadResult loadSymbolIndexValidated(const std::filesystem::path& indexFile);
[[nodiscard]] ValidationPolicy loadValidationPolicy(const std::filesystem::path& policyFile);
[[nodiscard]] std::string validationSeverity(std::string_view code, const ValidationPolicy& policy);
[[nodiscard]] std::vector<UnitMappingSuggestion> loadUnitMappingCatalog(
    const std::filesystem::path& catalogFile);
void enrichPackageMetadata(std::vector<IndexedUnit>& units,
                           const std::vector<std::filesystem::path>& packageRoots);
void mergeProjectSymbols(std::vector<IndexedUnit>& units, const AnalysisResult& project);
[[nodiscard]] std::vector<IndexedUnit> bundledSymbolIndex();
[[nodiscard]] std::string createSymbolIndex(const std::vector<std::filesystem::path>& sources);
[[nodiscard]] std::string createSymbolIndex(const std::vector<std::filesystem::path>& sources,
                                            std::string_view version);
[[nodiscard]] IndexBuildResult buildSymbolIndex(const std::vector<std::filesystem::path>& sources,
                                                const IndexBuildOptions& options = {});
[[nodiscard]] std::string indexBuildResultJson(const IndexBuildResult& result,
                                               const std::filesystem::path& output);

}  // namespace jdelphiast
