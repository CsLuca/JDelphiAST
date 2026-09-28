#pragma once

#include "jdelphiast/analyzer.hpp"

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

}  // namespace jdelphiast
