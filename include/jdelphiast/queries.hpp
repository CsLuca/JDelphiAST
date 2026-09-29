#pragma once

#include "jdelphiast/analyzer.hpp"
#include "jdelphiast/project.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace jdelphiast {

struct SymbolQuery {
  std::string name;
  std::string unit;
  std::string kind;
  std::string qualifiedName;
};

struct UnitMapOptions {
  std::filesystem::path leftIndex;
  std::filesystem::path rightIndex;
  std::filesystem::path seedFile;
  bool includeUnmapped{};
  bool includeAmbiguous{};
  bool includeSymbolDetails{};
  std::string minimumConfidence{"low"};
  std::filesystem::path validationPolicy;
  std::vector<ValidationDiagnostic> validationDiagnostics;
};

struct UnitMapOutput {
  std::string json;
  std::string csv;
  std::string html;
  bool hasBlockingErrors{};
};

struct DiagnoseOptions {
  std::filesystem::path file;
  std::filesystem::path dproj;
  std::filesystem::path unitMap;
  std::string configuration{"Release"};
  std::string platform{"Win32"};
  std::string errorCode;
  std::string symbol;
  std::size_t line{};
  std::size_t timeoutMs{10000};
  QueryPerformance performance;
};

[[nodiscard]] std::string exportsJson(const std::filesystem::path& unit,
                                      const std::vector<IndexedUnit>& index);
[[nodiscard]] std::string symbolJson(const SymbolQuery& query,
                                     const std::vector<IndexedUnit>& index,
                                     QueryPerformance performance = {});
[[nodiscard]] std::string unitInfoJson(std::string_view unit,
                                       const std::vector<IndexedUnit>& index,
                                       QueryPerformance performance = {});
[[nodiscard]] std::string expressionJson(const std::filesystem::path& file, std::size_t line,
                                         const std::vector<IndexedUnit>& index);
[[nodiscard]] std::string hierarchyJson(const std::filesystem::path& file, std::string_view className,
                                        const std::vector<IndexedUnit>& index);
[[nodiscard]] std::string compareSymbolJson(std::string_view symbol,
                                            const std::vector<IndexedUnit>& left,
                                            const std::vector<IndexedUnit>& right);
[[nodiscard]] std::string diagnoseJson(const DiagnoseOptions& options,
                                       const std::vector<IndexedUnit>& right,
                                       const std::vector<IndexedUnit>& left = {});
[[nodiscard]] std::string legacyReferencesJson(const std::filesystem::path& file,
                                                std::string_view legacyUnit,
                                                const std::vector<IndexedUnit>& left,
                                                const std::vector<IndexedUnit>& right,
                                                const std::filesystem::path& unitMap = {},
                                                QueryPerformance performance = {});
[[nodiscard]] std::string modelMigrationJson(const std::filesystem::path& file,
                                              std::string_view symbol,
                                              const std::vector<IndexedUnit>& left,
                                              const std::vector<IndexedUnit>& right,
                                              const std::filesystem::path& unitMap = {},
                                              QueryPerformance performance = {});
[[nodiscard]] std::string compilerLogJson(const std::filesystem::path& input,
                                          QueryPerformance performance = {});
[[nodiscard]] std::string typeInfoJson(std::string_view type,
                                       const std::vector<IndexedUnit>& index,
                                       bool includeInherited,
                                       bool includeOverloads,
                                       QueryPerformance performance = {});
[[nodiscard]] std::string symbolOriginJson(std::string_view name,
                                           const std::vector<IndexedUnit>& index,
                                           QueryPerformance performance = {});
[[nodiscard]] std::string batchDiagnoseJson(const std::filesystem::path& input,
                                            const std::vector<IndexedUnit>& right,
                                            const std::vector<IndexedUnit>& left,
                                            const std::filesystem::path& unitMap,
                                            std::size_t timeoutMs,
                                            QueryPerformance performance = {});
[[nodiscard]] UnitMapOutput compareUnits(const UnitMapOptions& options,
                                         const std::vector<IndexedUnit>& left,
                                         const std::vector<IndexedUnit>& right,
                                         std::string_view generatedAt);

}  // namespace jdelphiast
