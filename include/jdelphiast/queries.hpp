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

[[nodiscard]] std::string exportsJson(const std::filesystem::path& unit,
                                      const std::vector<IndexedUnit>& index);
[[nodiscard]] std::string symbolJson(const SymbolQuery& query,
                                     const std::vector<IndexedUnit>& index);
[[nodiscard]] std::string unitInfoJson(std::string_view unit,
                                       const std::vector<IndexedUnit>& index);
[[nodiscard]] std::string expressionJson(const std::filesystem::path& file, std::size_t line,
                                         const std::vector<IndexedUnit>& index);
[[nodiscard]] std::string hierarchyJson(const std::filesystem::path& file, std::string_view className,
                                        const std::vector<IndexedUnit>& index);
[[nodiscard]] std::string compareSymbolJson(std::string_view symbol,
                                            const std::vector<IndexedUnit>& left,
                                            const std::vector<IndexedUnit>& right);
[[nodiscard]] UnitMapOutput compareUnits(const UnitMapOptions& options,
                                         const std::vector<IndexedUnit>& left,
                                         const std::vector<IndexedUnit>& right,
                                         std::string_view generatedAt);

}  // namespace jdelphiast
