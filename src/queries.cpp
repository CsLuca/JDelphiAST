#include "jdelphiast/queries.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>

namespace jdelphiast {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string escape(std::string_view value) {
  std::string result;
  for (const unsigned char c : value) {
    if (c == '\\') result += "\\\\";
    else if (c == '"') result += "\\\"";
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else if (c < 0x20) {
      constexpr char digits[] = "0123456789abcdef";
      result += "\\u00";
      result += digits[c >> 4];
      result += digits[c & 15];
    } else result += static_cast<char>(c);
  }
  return result;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

const IndexedUnit* findUnit(const std::vector<IndexedUnit>& index, std::string_view name) {
  const auto key = lower(std::string(name));
  const auto found = std::find_if(index.begin(), index.end(), [&](const IndexedUnit& unit) {
    return lower(unit.name) == key;
  });
  return found == index.end() ? nullptr : &*found;
}

std::string signature(const AstDeclaration& declaration) {
  if (!declaration.signature.empty()) return declaration.signature;
  std::ostringstream output;
  output << declaration.kind << ' ' << declaration.name;
  if (!declaration.parameters.empty()) {
    output << '(';
    for (std::size_t i = 0; i < declaration.parameters.size(); ++i) {
      if (i) output << "; ";
      if (!declaration.parameters[i].modifier.empty()) output << declaration.parameters[i].modifier << ' ';
      output << declaration.parameters[i].name << ": " << declaration.parameters[i].type;
    }
    output << ')';
  }
  if (!declaration.type.empty()) output << ": " << declaration.type;
  return output.str();
}

void writeDiagnostic(std::ostringstream& output, std::string_view code, std::string_view message,
                     std::string_view file = {}, std::size_t line = 0) {
  output << "{\"code\":\"" << escape(code) << "\",\"severity\":\"warning\",\"message\":\""
         << escape(message) << "\"";
  if (!file.empty()) output << ",\"file\":\"" << escape(file) << "\"";
  if (line) output << ",\"line\":" << line;
  output << '}';
}

void writeDeclaration(std::ostringstream& output, const IndexedUnit& unit,
                      const AstDeclaration& declaration) {
  output << "{\"name\":\"" << escape(declaration.name) << "\",\"qualified_name\":\""
         << escape(unit.name + "." + declaration.name) << "\",\"kind\":\""
         << escape(declaration.kind) << "\",\"unit\":\"" << escape(unit.name)
         << "\",\"source_file\":";
  if (unit.sourceFile.empty()) output << "null";
  else output << '"' << escape(unit.sourceFile.generic_string()) << '"';
  output << ",\"visibility\":\"interface\",\"signature\":\"" << escape(signature(declaration)) << "\""
         << ",\"return_type\":";
  if (declaration.type.empty()) output << "null";
  else output << '"' << escape(declaration.type) << '"';
  output << ",\"parameters\":[";
  for (std::size_t i = 0; i < declaration.parameters.size(); ++i) {
    if (i) output << ',';
    const auto& parameter = declaration.parameters[i];
    output << "{\"position\":" << i + 1 << ",\"name\":\"" << escape(parameter.name)
           << "\",\"type\":\"" << escape(parameter.type) << "\",\"modifier\":\""
           << escape(parameter.modifier) << "\"}";
  }
  output << "],\"overload\":" << (declaration.overload ? "true" : "false")
         << ",\"package\":{\"name\":";
  if (unit.packageName.empty()) output << "null"; else output << '"' << escape(unit.packageName) << '"';
  output << ",\"dcp\":";
  if (unit.packageDcp.empty()) output << "null"; else output << '"' << escape(unit.packageDcp) << '"';
  output << ",\"bpl\":";
  if (unit.packageBpl.empty()) output << "null"; else output << '"' << escape(unit.packageBpl) << '"';
  output << "}}";
}

std::vector<const AstDeclaration*> matchingDeclarations(const IndexedUnit& unit, std::string_view name,
                                                        std::string_view kind = {}) {
  std::vector<const AstDeclaration*> result;
  const auto key = lower(std::string(name));
  for (const auto& declaration : unit.declarations) {
    const auto simple = declaration.name.substr(declaration.name.find_last_of('.') == std::string::npos
                                                    ? 0 : declaration.name.find_last_of('.') + 1);
    if (lower(simple) == key && (kind.empty() || lower(declaration.kind) == lower(std::string(kind))))
      result.push_back(&declaration);
  }
  return result;
}

}  // namespace

std::string exportsJson(const std::filesystem::path& requested, const std::vector<IndexedUnit>& index) {
  std::optional<UnitAst> parsed;
  const IndexedUnit* unit = nullptr;
  IndexedUnit sourceUnit;
  if (std::filesystem::exists(requested)) {
    const auto source = readFile(requested);
    parsed = parseUnit(requested, source);
    sourceUnit.name = parsed->name;
    sourceUnit.sourceFile = requested;
    sourceUnit.declarations = parsed->declarations;
    for (const auto& symbol : parsed->exports) {
      sourceUnit.symbols.push_back(symbol.name);
      const auto existing = std::find_if(sourceUnit.declarations.begin(), sourceUnit.declarations.end(), [&](const AstDeclaration& declaration) {
        return lower(declaration.name) == lower(symbol.name);
      });
      if (existing == sourceUnit.declarations.end()) {
        AstDeclaration declaration;
        declaration.name = symbol.name;
        declaration.kind = symbol.kind;
        declaration.visibility = "interface";
        declaration.range = symbol.range;
        sourceUnit.declarations.push_back(std::move(declaration));
      }
    }
    unit = &sourceUnit;
  } else unit = findUnit(index, requested.string());
  std::ostringstream output;
  output << "{\"schema_version\":\"2.0\",\"unit\":\"" << escape(unit ? unit->name : requested.string())
         << "\",\"source_file\":";
  if (!unit || unit->sourceFile.empty()) output << "null";
  else output << '"' << escape(unit->sourceFile.generic_string()) << '"';
  output << ",\"index_status\":\"" << (unit ? "indexed" : "not_found") << "\",\"exports\":[";
  if (unit) {
    bool first = true;
    for (const auto& name : unit->symbols) {
      if (!first) output << ',';
      first = false;
      const auto declarations = matchingDeclarations(*unit, name);
      output << "{\"name\":\"" << escape(name) << "\",\"qualified_name\":\""
             << escape(unit->name + "." + name) << "\",\"kind\":\""
             << escape(declarations.empty() ? "unknown" : declarations.front()->kind)
             << "\",\"visibility\":\"interface\",\"line\":"
             << (declarations.empty() ? 0 : declarations.front()->range.begin.line) << ",\"signature\":";
      if (declarations.empty()) output << "null";
      else output << '"' << escape(signature(*declarations.front())) << '"';
      if (!declarations.empty() && !declarations.front()->type.empty())
        output << ",\"type\":\"" << escape(declarations.front()->type) << '"';
      output << '}';
    }
  }
  output << "],\"diagnostics\":[";
  if (!unit) writeDiagnostic(output, "source_unit_not_indexed", "Unit not found in source or index.");
  output << "]}";
  return output.str();
}

std::string symbolJson(const SymbolQuery& query, const std::vector<IndexedUnit>& index) {
  std::ostringstream output;
  output << "{\"schema_version\":\"2.0\",\"query\":{\"name\":\"" << escape(query.name) << "\"},\"matches\":[";
  bool first = true;
  for (const auto& unit : index) {
    if (!query.unit.empty() && lower(unit.name) != lower(query.unit)) continue;
    for (const auto* declaration : matchingDeclarations(unit, query.name, query.kind)) {
      const auto qualified = unit.name + "." + declaration->name;
      if (!query.qualifiedName.empty() && lower(qualified) != lower(query.qualifiedName)) continue;
      if (!first) output << ',';
      first = false;
      writeDeclaration(output, unit, *declaration);
    }
    if (matchingDeclarations(unit, query.name, query.kind).empty() &&
        std::any_of(unit.symbols.begin(), unit.symbols.end(), [&](const std::string& symbol) {
          return lower(symbol) == lower(query.name);
        })) {
      AstDeclaration declaration;
      declaration.name = query.name;
      if (!query.kind.empty()) continue;
      declaration.kind = "unknown";
      if (!first) output << ',';
      first = false;
      writeDeclaration(output, unit, declaration);
    }
  }
  output << "],\"diagnostics\":[";
  if (first) writeDiagnostic(output, "unresolved_symbol", "Symbol not found in the persistent index.");
  output << "]}";
  return output.str();
}

std::string unitInfoJson(std::string_view requested, const std::vector<IndexedUnit>& index) {
  const auto* unit = findUnit(index, requested);
  std::ostringstream output;
  output << "{\"schema_version\":\"2.0\",\"unit\":\"" << escape(requested)
         << "\",\"normalized_unit\":\"" << escape(lower(std::string(requested))) << "\",\"source_file\":";
  if (!unit || unit->sourceFile.empty()) output << "null"; else output << '"' << escape(unit->sourceFile.generic_string()) << '"';
  output << ",\"index_status\":\"" << (unit ? "indexed" : "not_found") << "\",\"package\":{\"name\":";
  if (!unit || unit->packageName.empty()) output << "null"; else output << '"' << escape(unit->packageName) << '"';
  output << ",\"dcp\":";
  if (!unit || unit->packageDcp.empty()) output << "null"; else output << '"' << escape(unit->packageDcp) << '"';
  output << ",\"bpl\":";
  if (!unit || unit->packageBpl.empty()) output << "null"; else output << '"' << escape(unit->packageBpl) << '"';
  output << ",\"source_project\":";
  if (!unit || unit->sourceProject.empty()) output << "null"; else output << '"' << escape(unit->sourceProject.generic_string()) << '"';
  output << "},\"dependencies\":[";
  if (unit) for (std::size_t i = 0; i < unit->dependencies.size(); ++i) {
    if (i) output << ',';
    output << '"' << escape(unit->dependencies[i]) << '"';
  }
  output << "],\"search_path_hints\":[";
  if (unit && !unit->sourceFile.empty()) output << '"' << escape(unit->sourceFile.parent_path().generic_string()) << '"';
  output << "],\"diagnostics\":[";
  if (!unit) writeDiagnostic(output, "source_unit_not_indexed", "Unit not found in the persistent index.");
  else if (unit->packageName.empty()) writeDiagnostic(output, "package_metadata_not_available", "Package metadata is not available in the index.");
  output << "]}";
  return output.str();
}

std::string expressionJson(const std::filesystem::path& file, std::size_t line,
                           const std::vector<IndexedUnit>& index) {
  if (!std::filesystem::is_regular_file(file)) {
    std::ostringstream output;
    output << "{\"schema_version\":\"2.0\",\"file\":\"" << escape(file.generic_string())
           << "\",\"line\":" << line << ",\"expression\":\"\",\"resolved\":false,\"type\":null,"
              "\"receiver\":null,\"called_symbol\":null,\"confidence\":\"low\",\"diagnostics\":[";
    writeDiagnostic(output, "source_file_not_found", "Source file was not found.", file.generic_string(), line);
    output << "]}";
    return output.str();
  }
  const auto source = readFile(file);
  std::istringstream lines(source);
  std::string text;
  for (std::size_t current = 1; current <= line && std::getline(lines, text); ++current)
    if (current != line) text.clear();
  std::string called;
  const auto open = text.find('(');
  if (open != std::string::npos) {
    auto begin = open;
    while (begin > 0 && (std::isalnum(static_cast<unsigned char>(text[begin - 1])) || text[begin - 1] == '_' || text[begin - 1] == '.')) --begin;
    called = text.substr(begin, open - begin);
  }
  std::vector<std::pair<const IndexedUnit*, const AstDeclaration*>> matches;
  std::string receiverType;
  const auto ast = parseUnit(file, source);
  const auto firstDot = called.find('.');
  if (firstDot != std::string::npos) {
    const auto receiver = called.substr(0, firstDot);
    const auto declared = std::find_if(ast.declarations.begin(), ast.declarations.end(), [&](const AstDeclaration& declaration) {
      return lower(declaration.name) == lower(receiver) && !declaration.type.empty();
    });
    if (declared != ast.declarations.end()) receiverType = declared->type;
  }
  if (!called.empty()) {
    const auto simple = called.substr(called.find_last_of('.') == std::string::npos ? 0 : called.find_last_of('.') + 1);
    for (const auto& unit : index) for (const auto* candidate : matchingDeclarations(unit, simple)) {
      if (candidate->type.empty()) continue;
      if (!receiverType.empty() && !candidate->ownerType.empty() && lower(candidate->ownerType) != lower(receiverType)) continue;
      matches.push_back({&unit, candidate});
    }
  }
  bool visible = false;
  if (matches.size() == 1) {
    const auto ownerName = lower(matches.front().first->name);
    visible = std::any_of(ast.uses.begin(), ast.uses.end(), [&](const UsesItem& use) {
      const auto used = lower(use.name);
      return ownerName == used || (ownerName.size() > used.size() && ownerName.ends_with("." + used));
    });
  }
  const auto receiverProven = firstDot == std::string::npos || !receiverType.empty();
  const auto resolved = matches.size() == 1 && visible && receiverProven;
  const auto* owner = resolved ? matches.front().first : nullptr;
  const auto* match = resolved ? matches.front().second : nullptr;
  std::ostringstream output;
  output << "{\"schema_version\":\"2.0\",\"file\":\"" << escape(file.generic_string())
         << "\",\"line\":" << line << ",\"expression\":\"" << escape(text)
         << "\",\"resolved\":" << (resolved ? "true" : "false") << ",\"type\":";
  if (!match) output << "null";
  else output << "{\"name\":\"" << escape(match->type) << "\",\"qualified_name\":\""
              << escape(match->type) << "\",\"declared_in_unit\":\"" << escape(owner->name)
              << "\",\"source_file\":" << (owner->sourceFile.empty() ? "null" : "\"" + escape(owner->sourceFile.generic_string()) + "\"") << '}';
  output << ",\"receiver\":";
  if (firstDot == std::string::npos) output << "null";
  else output << "{\"expression\":\"" << escape(called.substr(0, firstDot)) << "\",\"type\":"
              << (receiverType.empty() ? "null" : "\"" + escape(receiverType) + "\"") << '}';
  output << ",\"called_symbol\":";
  if (!match) output << "null";
  else output << "{\"qualified_name\":\"" << escape(owner->name + "." + match->name)
              << "\",\"signature\":\"" << escape(signature(*match)) << "\"}";
  output << ",\"confidence\":\"" << (resolved ? "medium" : "low") << "\",\"diagnostics\":[";
  if (!resolved) writeDiagnostic(output, matches.size() > 1 ? "ambiguous_symbol" : "expression_type_unresolved",
                                 matches.size() > 1 ? "More than one callable candidate matches the expression." : "Expression type could not be resolved from visible symbols.",
                                 file.generic_string(), line);
  output << "]}";
  return output.str();
}

std::string hierarchyJson(const std::filesystem::path& file, std::string_view className,
                          const std::vector<IndexedUnit>& index) {
  if (!std::filesystem::is_regular_file(file)) {
    std::ostringstream output;
    output << "{\"schema_version\":\"2.0\",\"class\":{\"name\":\"" << escape(className)
           << "\",\"qualified_name\":null,\"source_file\":\"" << escape(file.generic_string())
           << "\"},\"base_class\":null,\"ancestors\":[],\"methods\":[],\"diagnostics\":[";
    writeDiagnostic(output, "source_file_not_found", "Source file was not found.", file.generic_string());
    output << "]}";
    return output.str();
  }
  const auto source = readFile(file);
  const auto ast = source.empty() ? UnitAst{} : parseUnit(file, source);
  const InheritanceRelation* relation = nullptr;
  for (const auto& candidate : ast.inheritance) if (lower(candidate.type) == lower(std::string(className))) relation = &candidate;
  std::ostringstream output;
  output << "{\"schema_version\":\"2.0\",\"class\":{\"name\":\"" << escape(className)
         << "\",\"qualified_name\":\"" << escape(ast.name + "." + std::string(className))
         << "\",\"source_file\":\"" << escape(file.generic_string()) << "\"},\"base_class\":";
  if (!relation) output << "null";
  else output << "{\"name\":\"" << escape(relation->baseType) << "\",\"qualified_name\":\""
              << escape(relation->baseType) << "\"}";
  output << ",\"ancestors\":[";
  if (relation && !relation->baseType.empty()) output << '"' << escape(relation->baseType) << '"';
  output << "],\"methods\":[";
  bool firstMethod = true;
  bool incompatible = false;
  if (relation) {
    std::vector<const AstDeclaration*> baseMethods;
    for (const auto& unit : index) for (const auto& declaration : unit.declarations)
      if (lower(declaration.name).starts_with(lower(relation->baseType + "."))) baseMethods.push_back(&declaration);
    for (const auto& local : ast.declarations) {
      if (!lower(local.name).starts_with(lower(std::string(className) + "."))) continue;
      const auto methodName = local.name.substr(local.name.find_last_of('.') + 1);
      std::vector<const AstDeclaration*> sameName;
      for (const auto* candidate : baseMethods)
        if (lower(candidate->name.substr(candidate->name.find_last_of('.') + 1)) == lower(methodName)) sameName.push_back(candidate);
      if (sameName.size() != 1) continue;
      const auto* base = sameName.front();
      std::vector<std::size_t> differences;
      const auto count = std::max(local.parameters.size(), base->parameters.size());
      for (std::size_t p = 0; p < count; ++p)
        if (p >= local.parameters.size() || p >= base->parameters.size() ||
            lower(local.parameters[p].type) != lower(base->parameters[p].type) ||
            lower(local.parameters[p].modifier) != lower(base->parameters[p].modifier)) differences.push_back(p);
      if (!firstMethod) output << ',';
      firstMethod = false;
      incompatible = incompatible || (local.isOverride && !differences.empty());
      output << "{\"name\":\"" << escape(methodName) << "\",\"kind\":\"" << escape(local.kind)
             << "\",\"local_declaration\":\"" << escape(signature(local))
             << "\",\"local_implementation\":\"" << escape(signature(local))
             << "\",\"base_declaration\":\"" << escape(signature(*base))
             << "\",\"is_override\":" << (local.isOverride ? "true" : "false") << ",\"override_status\":\""
             << (!local.isOverride ? "unknown" : differences.empty() ? "compatible" : "incompatible")
             << "\",\"parameter_differences\":[";
      for (std::size_t d = 0; d < differences.size(); ++d) {
        if (d) output << ',';
        const auto p = differences[d];
        output << "{\"position\":" << p + 1 << ",\"local_type\":\""
               << escape(p < local.parameters.size() ? local.parameters[p].type : "<missing>")
               << "\",\"base_type\":\""
               << escape(p < base->parameters.size() ? base->parameters[p].type : "<missing>") << "\"}";
      }
      output << "]}";
    }
  }
  output << "],\"diagnostics\":[";
  bool wroteDiagnostic = false;
  if (!relation) { writeDiagnostic(output, "inheritance_unresolved", "Class inheritance could not be resolved.", file.generic_string()); wroteDiagnostic = true; }
  if (incompatible) {
    if (wroteDiagnostic) output << ',';
    writeDiagnostic(output, "override_signature_incompatible", "A local method signature differs from its base declaration.", file.generic_string());
  }
  output << "]}";
  (void)index;
  return output.str();
}

std::string compareSymbolJson(std::string_view symbol, const std::vector<IndexedUnit>& left,
                              const std::vector<IndexedUnit>& right) {
  const auto* leftUnit = findUnit(left, symbol);
  if (!leftUnit) {
    for (const auto& unit : left)
      if (std::any_of(unit.symbols.begin(), unit.symbols.end(), [&](const std::string& candidate) {
            return lower(candidate) == lower(std::string(symbol));
          })) { leftUnit = &unit; break; }
  }
  std::vector<std::string> leftExports;
  if (leftUnit) leftExports = leftUnit->symbols;
  std::ostringstream output;
  output << "{\"schema_version\":\"2.0\",\"left\":{\"version\":\""
         << escape(leftUnit ? leftUnit->indexVersion : "unknown") << "\",\"unit\":\""
         << escape(leftUnit ? leftUnit->name : std::string(symbol)) << "\",\"exports\":[";
  for (std::size_t i = 0; i < leftExports.size(); ++i) {
    if (i) output << ',';
    const auto declarations = matchingDeclarations(*leftUnit, leftExports[i]);
    output << "{\"name\":\"" << escape(leftExports[i]) << "\",\"kind\":\""
           << escape(declarations.empty() ? "unknown" : declarations.front()->kind) << "\"}";
  }
  output << "]},\"right_candidates\":[";
  bool first = true;
  if (leftUnit) for (const auto& unit : right) {
    std::vector<std::string> matching, missing;
    bool declarationsCompatible = true;
    for (const auto& exported : leftExports) {
      const auto found = std::any_of(unit.symbols.begin(), unit.symbols.end(), [&](const std::string& candidate) {
        return lower(candidate) == lower(exported);
      });
      (found ? matching : missing).push_back(exported);
      if (found) {
        const auto leftDeclarations = matchingDeclarations(*leftUnit, exported);
        const auto rightDeclarations = matchingDeclarations(unit, exported);
        if (!leftDeclarations.empty() && !rightDeclarations.empty() &&
            (lower(leftDeclarations.front()->kind) != lower(rightDeclarations.front()->kind) ||
             (!leftDeclarations.front()->signature.empty() && !rightDeclarations.front()->signature.empty() &&
              lower(leftDeclarations.front()->signature) != lower(rightDeclarations.front()->signature))))
          declarationsCompatible = false;
      }
    }
    const auto overlap = leftExports.empty() ? 0.0 : static_cast<double>(matching.size()) / leftExports.size();
    const auto sameUnitName = lower(unit.name) == lower(leftUnit->name);
    if (matching.empty() || (!sameUnitName && (matching.size() < 2 || overlap < 0.5))) continue;
    if (!first) output << ',';
    first = false;
    output << "{\"unit\":\"" << escape(unit.name) << "\",\"source_file\":";
    if (unit.sourceFile.empty()) output << "null"; else output << '"' << escape(unit.sourceFile.generic_string()) << '"';
    output << ",\"matching_exports\":[";
    for (std::size_t i = 0; i < matching.size(); ++i) { if (i) output << ','; output << '"' << escape(matching[i]) << '"'; }
    output << "],\"missing_exports\":[";
    for (std::size_t i = 0; i < missing.size(); ++i) { if (i) output << ','; output << '"' << escape(missing[i]) << '"'; }
    const auto compatibility = !declarationsCompatible ? "partial_compatible"
                              : missing.empty() ? "full_compatible" : "partial_compatible";
    const auto confidence = overlap >= 0.5 && matching.size() >= 2 && declarationsCompatible ? "high" : "low";
    output << "],\"compatibility\":\"" << compatibility
           << "\",\"confidence\":\"" << confidence << "\"}";
  }
  output << "],\"diagnostics\":[";
  if (!leftUnit) writeDiagnostic(output, "legacy_symbol_unmapped", "Left unit or symbol was not found.");
  else if (first) writeDiagnostic(output, "legacy_symbol_unmapped", "No verified mapping was found in the right index.");
  output << "]}";
  return output.str();
}

}  // namespace jdelphiast
