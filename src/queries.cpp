#include "jdelphiast/queries.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

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

struct SeedMapping {
  std::string left;
  std::string right;
  std::string type;
  std::string confidence;
  std::string action;
  std::string notes;
  bool nullTarget{};
};

class SeedJsonParser {
 public:
  explicit SeedJsonParser(std::string_view source) : source_(source) {}

  std::vector<SeedMapping> parse() {
    expect('{');
    std::string schema;
    std::vector<SeedMapping> mappings;
    bool sawMappings = false;
    while (!consume('}')) {
      const auto key = string(); expect(':');
      if (key == "schema_version") schema = string();
      else if (key == "mappings") { sawMappings = true; mappings = mappingArray(); }
      else skipValue();
      if (!consume(',')) expect('}'); else continue;
      break;
    }
    skip();
    if (position_ != source_.size() || schema != "1.0" || !sawMappings || mappings.empty())
      throw std::runtime_error("Invalid seed mapping schema");
    return mappings;
  }

 private:
  std::vector<SeedMapping> mappingArray() {
    std::vector<SeedMapping> result;
    expect('[');
    if (consume(']')) return result;
    while (true) {
      result.push_back(mapping());
      if (consume(']')) break;
      expect(',');
    }
    return result;
  }
  SeedMapping mapping() {
    SeedMapping value;
    expect('{');
    while (!consume('}')) {
      const auto key = string(); expect(':');
      if (key == "v600_unit" && literal("null")) value.nullTarget = true;
      else if (key == "v500_unit") value.left = string();
      else if (key == "v600_unit") value.right = string();
      else if (key == "mapping_type") value.type = string();
      else if (key == "confidence") value.confidence = string();
      else if (key == "automatic_action") value.action = string();
      else if (key == "notes") { if (!literal("null")) value.notes = string(); }
      else skipValue();
      if (!consume(',')) expect('}'); else continue;
      break;
    }
    return value;
  }
  std::string string() {
    skip(); expectRaw('"');
    std::string value;
    while (position_ < source_.size() && source_[position_] != '"') {
      if (source_[position_] == '\\') {
        if (++position_ >= source_.size()) fail();
        const char escaped = source_[position_++];
        if (escaped == 'n') value += '\n'; else if (escaped == 'r') value += '\r';
        else if (escaped == 't') value += '\t'; else if (escaped == '"' || escaped == '\\' || escaped == '/') value += escaped;
        else fail();
      } else value += source_[position_++];
    }
    expectRaw('"');
    return value;
  }
  void skipValue() {
    skip();
    if (position_ >= source_.size()) fail();
    if (source_[position_] == '"') { (void)string(); return; }
    if (source_[position_] == '{' || source_[position_] == '[') {
      const char open = source_[position_++], close = open == '{' ? '}' : ']';
      int depth = 1; bool quoted = false;
      while (position_ < source_.size() && depth) {
        const char c = source_[position_++];
        if (quoted && c == '\\') { if (position_ < source_.size()) ++position_; continue; }
        if (c == '"') quoted = !quoted;
        else if (!quoted && c == open) ++depth;
        else if (!quoted && c == close) --depth;
      }
      if (depth) fail();
      return;
    }
    while (position_ < source_.size() && source_[position_] != ',' && source_[position_] != '}' && source_[position_] != ']') ++position_;
  }
  bool literal(std::string_view value) {
    skip();
    if (source_.substr(position_, value.size()) != value) return false;
    position_ += value.size(); return true;
  }
  bool consume(char value) { skip(); if (position_ < source_.size() && source_[position_] == value) { ++position_; return true; } return false; }
  void expect(char value) { skip(); expectRaw(value); }
  void expectRaw(char value) { if (position_ >= source_.size() || source_[position_] != value) fail(); ++position_; }
  void skip() { while (position_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[position_]))) ++position_; }
  [[noreturn]] void fail() const { throw std::runtime_error("Invalid seed mapping JSON"); }
  std::string_view source_; std::size_t position_{};
};

std::vector<SeedMapping> loadSeed(const std::filesystem::path& file) {
  if (file.empty()) return {};
  const auto source = readFile(file);
  if (source.empty()) throw std::runtime_error("Seed mapping file is missing or empty: " + file.string());
  auto result = SeedJsonParser(source).parse();
  std::set<std::string> seen;
  for (const auto& mapping : result) {
    if (mapping.left.empty() || mapping.type.empty() || mapping.confidence.empty() || mapping.action.empty())
      throw std::runtime_error("Incomplete seed mapping in " + file.string());
    const auto key = lower(mapping.left);
    if (!seen.insert(key).second) throw std::runtime_error("Duplicate seed mapping for " + mapping.left);
    if (mapping.confidence != "low" && mapping.confidence != "medium" && mapping.confidence != "high")
      throw std::runtime_error("Invalid seed confidence for " + mapping.left);
    const std::set<std::string> types = {"unit_rename", "namespace_migration", "partial_compatibility",
        "relocated_symbols", "semantic_migration_required", "removed_no_equivalent", "ambiguous", "not_found"};
    const std::set<std::string> actions = {"replace_in_uses", "conditional_replace_in_uses", "symbol_by_symbol",
        "manual_required", "do_not_remove", "review_required"};
    if (!types.contains(mapping.type) || !actions.contains(mapping.action))
      throw std::runtime_error("Invalid seed mapping enum for " + mapping.left);
    if (mapping.nullTarget && mapping.type != "relocated_symbols" && mapping.type != "semantic_migration_required" &&
        mapping.type != "removed_no_equivalent" && mapping.type != "not_found")
      throw std::runtime_error("Seed mapping requires a V600 target for " + mapping.left);
  }
  return result;
}

std::vector<std::string> commonExports(const IndexedUnit& left, const IndexedUnit& right) {
  std::vector<std::string> result;
  for (const auto& symbol : left.symbols)
    if (std::any_of(right.symbols.begin(), right.symbols.end(), [&](const std::string& candidate) {
          return lower(candidate) == lower(symbol);
        })) result.push_back(symbol);
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return lower(a) < lower(b); });
  return result;
}

std::vector<std::string> missingExports(const IndexedUnit& left, const IndexedUnit& right) {
  std::vector<std::string> result;
  for (const auto& symbol : left.symbols)
    if (std::none_of(right.symbols.begin(), right.symbols.end(), [&](const std::string& candidate) {
          return lower(candidate) == lower(symbol);
        })) result.push_back(symbol);
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return lower(a) < lower(b); });
  return result;
}

int confidenceRank(std::string_view confidence) {
  if (confidence == "high") return 3;
  if (confidence == "medium") return 2;
  return 1;
}

std::string csvEscape(std::string_view value) {
  std::string result = "\"";
  for (const char c : value) { if (c == '"') result += '"'; result += c; }
  return result + '"';
}

std::string htmlEscape(std::string_view value) {
  std::string result;
  for (const char c : value) {
    if (c == '&') result += "&amp;";
    else if (c == '<') result += "&lt;";
    else if (c == '>') result += "&gt;";
    else if (c == '"') result += "&quot;";
    else if (c == '\'') result += "&#39;";
    else result += c;
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
  output << "{\"schema_version\":\"2.1\",\"unit\":\"" << escape(unit ? unit->name : requested.string())
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
  output << "],\"exports_summary\":{\"count\":" << (unit ? unit->symbols.size() : 0)
         << ",\"public_functions\":";
  std::size_t functions = 0, classes = 0, interfaces = 0;
  if (unit) for (const auto& declaration : unit->declarations) {
    if (declaration.kind == "function" || declaration.kind == "procedure") ++functions;
    else if (declaration.kind == "class") ++classes;
    else if (declaration.kind == "interface") ++interfaces;
  }
  output << functions << ",\"public_classes\":" << classes << ",\"public_interfaces\":" << interfaces << "}}";
  return output.str();
}

std::string symbolJson(const SymbolQuery& query, const std::vector<IndexedUnit>& index) {
  std::ostringstream output;
  output << "{\"schema_version\":\"2.1\",\"query\":{\"name\":\"" << escape(query.name) << "\"},\"matches\":[";
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
  std::vector<std::string> sourceEvidence;
  if (first && !query.name.empty()) {
    const std::regex token("\\b" + query.name + "\\b", std::regex::icase);
    for (const auto& unit : index) {
      if (unit.sourceFile.empty() || !std::filesystem::is_regular_file(unit.sourceFile)) continue;
      const auto source = readFile(unit.sourceFile);
      if (std::regex_search(source, token)) sourceEvidence.push_back(unit.sourceFile.generic_string());
      if (sourceEvidence.size() >= 8) break;
    }
  }
  output << "],\"source_evidence\":[";
  for (std::size_t i = 0; i < sourceEvidence.size(); ++i) {
    if (i) output << ',';
    output << '"' << escape(sourceEvidence[i]) << '"';
  }
  output << "],\"classification\":";
  if (!sourceEvidence.empty()) output << "\"ambiguous\",\"recommended_action\":\"review_required\",\"reason\":\"symbol_exists_in_v600_but_owner_unit_is_ambiguous\"";
  else output << "null,\"recommended_action\":\"none\",\"reason\":null";
  output << ",\"diagnostics\":[";
  if (first) writeDiagnostic(output, sourceEvidence.empty() ? "unresolved_symbol" : "ambiguous_symbol_owner",
                             sourceEvidence.empty() ? "Symbol not found in the persistent index."
                                                    : "Symbol exists in indexed V600 sources but its owner unit is ambiguous.");
  output << "]}";
  return output.str();
}

std::string unitInfoJson(std::string_view requested, const std::vector<IndexedUnit>& index) {
  const auto* unit = findUnit(index, requested);
  std::ostringstream output;
  output << "{\"schema_version\":\"2.1\",\"unit\":\"" << escape(requested)
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
  output << "],\"build_classification\":";
  if (unit) output << "\"build_path_required\""; else output << "null";
  output << ",\"search_path_hints\":[";
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
    output << "{\"schema_version\":\"2.1\",\"file\":\"" << escape(file.generic_string())
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
  output << "{\"schema_version\":\"2.1\",\"file\":\"" << escape(file.generic_string())
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
    output << "{\"schema_version\":\"2.1\",\"class\":{\"name\":\"" << escape(className)
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
  output << "{\"schema_version\":\"2.1\",\"class\":{\"name\":\"" << escape(className)
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
  output << "{\"schema_version\":\"2.1\",\"left\":{\"version\":\""
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
    for (std::size_t i = 0; i < unit.symbols.size(); ++i) { if (i) output << ','; output << '"' << escape(unit.symbols[i]) << '"'; }
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

UnitMapOutput compareUnits(const UnitMapOptions& options, const std::vector<IndexedUnit>& leftInput,
                           const std::vector<IndexedUnit>& rightInput, std::string_view generatedAt) {
  struct Mapping {
    const IndexedUnit* left{};
    const IndexedUnit* right{};
    std::string type, compatibility, confidence, action, notes;
    std::vector<std::string> matched, missing, incompatible, candidates, diagnostics;
    int nameScore{}, signatureMatches{}, signatureIncompatible{};
  };
  auto left = leftInput;
  auto right = rightInput;
  const auto severityPolicy = loadValidationPolicy(options.validationPolicy);
  const auto diagnosticSeverity = [&](std::string_view code) {
    return validationSeverity(code, severityPolicy);
  };
  const auto byName = [](const IndexedUnit& a, const IndexedUnit& b) {
    const auto leftName = lower(a.name), rightName = lower(b.name);
    return leftName == rightName ? a.name < b.name : leftName < rightName;
  };
  std::sort(left.begin(), left.end(), byName);
  std::sort(right.begin(), right.end(), byName);
  std::set<std::string> duplicateLeft, duplicateRight;
  for (std::size_t i = 1; i < left.size(); ++i)
    if (lower(left[i - 1].name) == lower(left[i].name)) duplicateLeft.insert(lower(left[i].name));
  for (std::size_t i = 1; i < right.size(); ++i)
    if (lower(right[i - 1].name) == lower(right[i].name)) duplicateRight.insert(lower(right[i].name));
  std::unordered_map<std::string, std::vector<const IndexedUnit*>> rightBySimpleName;
  std::unordered_map<std::string, std::vector<const IndexedUnit*>> rightByExport;
  for (const auto& unit : right) {
    const auto separator = unit.name.find_last_of('.');
    rightBySimpleName[lower(unit.name.substr(separator == std::string::npos ? 0 : separator + 1))].push_back(&unit);
    for (const auto& symbol : unit.symbols) rightByExport[lower(symbol)].push_back(&unit);
  }
  std::vector<SeedMapping> seeds;
  std::vector<ValidationDiagnostic> earlyDiagnostics = options.validationDiagnostics;
  if (!options.seedFile.empty()) {
    try { seeds = loadSeed(options.seedFile); }
    catch (const std::exception& error) {
      const std::string message = error.what();
      const auto code = message.find("missing or empty") != std::string::npos ? "seed_missing"
                      : message.find("schema") != std::string::npos ? "seed_schema_invalid" : "seed_parse_failed";
      earlyDiagnostics.push_back({code, diagnosticSeverity(code), message, options.seedFile, {}});
    }
  }
  const std::unordered_map<std::string, std::string> namespaces = {
      {"sysutils", "System.SysUtils"}, {"classes", "System.Classes"}, {"forms", "Vcl.Forms"},
      {"comctrls", "Vcl.ComCtrls"}, {"actnlist", "Vcl.ActnList"}, {"db", "Data.DB"},
      {"generics.collections", "System.Generics.Collections"}, {"comobj", "System.Win.ComObj"},
      {"clipbrd", "Vcl.Clipbrd"}};
  const std::set<std::string> semanticOnly = {"csmail", "rnote", "uvaristd", "iacquisti", "iquoart",
      "icontatti", "ustamain", "cbfrig", "fsecstdns", "fgridstdns", "cercaprezzostd"};
  std::vector<Mapping> mappings;
  for (const auto& leftUnit : left) {
    if (!mappings.empty() && lower(mappings.back().left->name) == lower(leftUnit.name)) continue;
    Mapping mapping;
    mapping.left = &leftUnit;
    if (duplicateLeft.contains(lower(leftUnit.name))) {
      mapping.type = "ambiguous";
      mapping.compatibility = "ambiguous";
      mapping.confidence = "low";
      mapping.action = "review_required";
      mapping.diagnostics.push_back("duplicate_unit_source");
      for (const auto& candidate : left)
        if (lower(candidate.name) == lower(leftUnit.name))
          mapping.candidates.push_back(candidate.sourceFile.empty() ? candidate.name : candidate.sourceFile.generic_string());
      mappings.push_back(std::move(mapping));
      continue;
    }
    const auto seed = std::find_if(seeds.begin(), seeds.end(), [&](const SeedMapping& item) {
      return lower(item.left) == lower(leftUnit.name);
    });
    if (seed != seeds.end()) {
      mapping.type = "seeded_mapping";
      mapping.compatibility = seed->type == "partial_compatibility" ? "partial_compatible"
                            : seed->type == "relocated_symbols" ? "relocated_symbols"
                            : seed->type == "semantic_migration_required" ? "semantic_migration_required"
                            : seed->type == "removed_no_equivalent" ? "not_found" : "full_compatible";
      mapping.confidence = seed->confidence.empty() ? "high" : seed->confidence;
      mapping.action = seed->action.empty() ? "review_required" : seed->action;
      mapping.notes = seed->notes;
      if (!seed->nullTarget) mapping.right = findUnit(right, seed->right);
      if (mapping.right && duplicateRight.contains(lower(mapping.right->name))) {
        mapping.type = "ambiguous";
        mapping.compatibility = "ambiguous";
        mapping.confidence = "low";
        mapping.action = "review_required";
        mapping.diagnostics.push_back("duplicate_unit_source");
        for (const auto& candidate : right)
          if (lower(candidate.name) == lower(mapping.right->name))
            mapping.candidates.push_back(candidate.sourceFile.empty() ? candidate.name : candidate.sourceFile.generic_string());
        mapping.right = nullptr;
      }
      if (!seed->nullTarget && !mapping.right && mapping.type != "ambiguous") {
        mapping.type = "not_found";
        mapping.compatibility = "not_found";
        mapping.confidence = "low";
        mapping.action = "review_required";
        mapping.diagnostics.push_back("seed_target_not_found");
      }
    } else if (semanticOnly.contains(lower(leftUnit.name))) {
      mapping.type = lower(leftUnit.name) == "rnote" ? "relocated_symbols" : "semantic_migration_required";
      mapping.compatibility = mapping.type;
      mapping.confidence = "high";
      mapping.action = mapping.type == "relocated_symbols" ? "symbol_by_symbol" : "manual_required";
    } else {
      mapping.right = findUnit(right, leftUnit.name);
      if (mapping.right && duplicateRight.contains(lower(mapping.right->name))) {
        mapping.type = "ambiguous";
        mapping.compatibility = "ambiguous";
        mapping.confidence = "low";
        mapping.action = "review_required";
        mapping.diagnostics.push_back("duplicate_unit_source");
        for (const auto& candidate : right)
          if (lower(candidate.name) == lower(mapping.right->name))
            mapping.candidates.push_back(candidate.sourceFile.empty() ? candidate.name : candidate.sourceFile.generic_string());
        mapping.right = nullptr;
      }
      if (mapping.right) mapping.nameScore = 100;
      if (!mapping.right && mapping.type.empty()) {
        const auto known = namespaces.find(lower(leftUnit.name));
        if (known != namespaces.end()) { mapping.right = findUnit(right, known->second); mapping.nameScore = 95; }
      }
      if (!mapping.right && mapping.type.empty()) {
        const auto simple = lower(leftUnit.name.substr(leftUnit.name.find_last_of('.') == std::string::npos
                                                          ? 0 : leftUnit.name.find_last_of('.') + 1));
        std::vector<const IndexedUnit*> candidates;
        std::unordered_map<const IndexedUnit*, std::size_t> exportMatches;
        for (const auto& symbol : leftUnit.symbols)
          if (const auto owners = rightByExport.find(lower(symbol)); owners != rightByExport.end())
            for (const auto* owner : owners->second) ++exportMatches[owner];
        std::unordered_set<const IndexedUnit*> candidateSet;
        if (const auto sameName = rightBySimpleName.find(simple); sameName != rightBySimpleName.end())
          candidateSet.insert(sameName->second.begin(), sameName->second.end());
        for (const auto& [candidate, matches] : exportMatches)
          if (matches >= 2) candidateSet.insert(candidate);
        for (const auto& candidate : right)
          if (candidateSet.contains(&candidate)) candidates.push_back(&candidate);
        if (candidates.size() == 1) mapping.right = candidates.front();
        else if (candidates.size() > 1) {
          mapping.type = "ambiguous"; mapping.compatibility = "ambiguous"; mapping.confidence = "low";
          mapping.action = "review_required";
          for (const auto* candidate : candidates) mapping.candidates.push_back(candidate->name);
        }
      }
      if (!mapping.right && mapping.type.empty() && leftUnit.symbols.size() >= 2) {
        std::set<std::string> relocatedUnits;
        std::size_t relocatedExports = 0;
        for (const auto& exported : leftUnit.symbols) {
          const auto owners = rightByExport.find(lower(exported));
          if (owners != rightByExport.end() && owners->second.size() == 1) {
            relocatedUnits.insert(owners->second.front()->name);
            ++relocatedExports;
          }
        }
        if (relocatedUnits.size() > 1 && relocatedExports >= 2) {
          mapping.type = "relocated_symbols";
          mapping.compatibility = "relocated_symbols";
          mapping.confidence = relocatedExports == leftUnit.symbols.size() ? "high" : "medium";
          mapping.action = "symbol_by_symbol";
          mapping.candidates.assign(relocatedUnits.begin(), relocatedUnits.end());
        }
      }
      if (mapping.right) {
        mapping.matched = commonExports(leftUnit, *mapping.right);
        mapping.missing = missingExports(leftUnit, *mapping.right);
        for (const auto& name : mapping.matched) {
          const auto l = matchingDeclarations(leftUnit, name);
          const auto r = matchingDeclarations(*mapping.right, name);
          if (!l.empty() && !r.empty() && !l.front()->signature.empty() && !r.front()->signature.empty()) {
            if (lower(l.front()->signature) == lower(r.front()->signature)) ++mapping.signatureMatches;
            else { ++mapping.signatureIncompatible; mapping.incompatible.push_back(name); }
          }
        }
        const auto complete = !leftUnit.symbols.empty() && mapping.missing.empty() && mapping.signatureIncompatible == 0;
        const auto nameEvidence = mapping.nameScore >= 95;
        const auto signatureEvidence = mapping.signatureMatches > 0;
        mapping.type = complete ? (mapping.nameScore == 95 ? "namespace_migration" : "unit_rename") : "partial_compatibility";
        mapping.compatibility = complete ? "full_compatible" : "partial_compatible";
        mapping.confidence = complete && (nameEvidence || signatureEvidence) ? "high" : "medium";
        mapping.action = mapping.confidence == "high" && complete ? "replace_in_uses" : "review_required";
      } else if (mapping.type.empty()) {
        mapping.type = "not_found"; mapping.compatibility = "not_found"; mapping.confidence = "low";
        mapping.action = "do_not_remove";
      }
    }
    if (mapping.right && mapping.matched.empty()) {
      mapping.matched = commonExports(leftUnit, *mapping.right);
      mapping.missing = missingExports(leftUnit, *mapping.right);
    }
    if (seed != seeds.end() && mapping.right) {
      for (const auto& name : mapping.matched) {
        const auto leftDeclarations = matchingDeclarations(leftUnit, name);
        const auto rightDeclarations = matchingDeclarations(*mapping.right, name);
        if (!leftDeclarations.empty() && !rightDeclarations.empty() &&
            !leftDeclarations.front()->signature.empty() && !rightDeclarations.front()->signature.empty()) {
          if (lower(leftDeclarations.front()->signature) == lower(rightDeclarations.front()->signature)) ++mapping.signatureMatches;
          else { ++mapping.signatureIncompatible; mapping.incompatible.push_back(name); }
        }
      }
      const auto seedAllowsPartial = seed->type == "partial_compatibility" && seed->action == "conditional_replace_in_uses";
      if (mapping.matched.empty() || mapping.signatureIncompatible > 0 || (!mapping.missing.empty() && !seedAllowsPartial)) {
        mapping.confidence = mapping.signatureIncompatible > 0 ? "low" : "medium";
        mapping.action = "review_required";
        mapping.diagnostics.push_back(mapping.signatureIncompatible > 0 ? "seed_signature_incompatible"
                                      : mapping.matched.empty() ? "seed_without_export_evidence" : "seed_missing_exports");
      }
    }
    mappings.push_back(std::move(mapping));
  }
  const auto minimumRank = confidenceRank(options.minimumConfidence);
  std::vector<const Mapping*> emitted;
  for (const auto& mapping : mappings) {
    if (confidenceRank(mapping.confidence) < minimumRank) continue;
    if (mapping.compatibility == "not_found" && !options.includeUnmapped) continue;
    if (mapping.type == "ambiguous" && !options.includeAmbiguous) continue;
    emitted.push_back(&mapping);
  }
  std::unordered_map<std::string, int> statistics;
  for (const auto& mapping : mappings) ++statistics[mapping.compatibility];
  int validationErrors = 0;
  int validationWarnings = 0;
  std::vector<ValidationDiagnostic> validationDiagnostics = std::move(earlyDiagnostics);
  for (const auto& mapping : mappings) {
    for (const auto& diagnostic : mapping.diagnostics) {
      validationDiagnostics.push_back({diagnostic, diagnosticSeverity(diagnostic), diagnostic, {}, mapping.left->name});
    }
  }
  for (auto& diagnostic : validationDiagnostics) {
    diagnostic.severity = validationSeverity(diagnostic.code, severityPolicy);
    if (diagnostic.severity == "error") ++validationErrors;
    else ++validationWarnings;
  }
  if (validationErrors > 0)
    validationDiagnostics.push_back({"output_not_written", "info",
        "Output files were not written because validation contains blocking errors.", {}, {}});
  std::ostringstream json;
  json << "{\"schema_version\":\"1.0\",\"generated_at\":\"" << escape(generatedAt)
       << "\",\"left_catalog\":{\"version\":\"" << escape(left.empty() ? "unknown" : left.front().indexVersion)
       << "\",\"index\":\"" << escape(options.leftIndex.generic_string()) << "\"},\"right_catalog\":{\"version\":\""
       << escape(right.empty() ? "unknown" : right.front().indexVersion) << "\",\"index\":\""
       << escape(options.rightIndex.generic_string()) << "\"},\"statistics\":{\"v500_units\":" << left.size()
       << ",\"v600_units\":" << right.size() << ",\"full_compatible\":" << statistics["full_compatible"]
       << ",\"partial_compatible\":" << statistics["partial_compatible"]
       << ",\"relocated_symbols\":" << statistics["relocated_symbols"]
      << ",\"semantic_migration_required\":" << statistics["semantic_migration_required"]
       << ",\"ambiguous\":" << statistics["ambiguous"] << ",\"not_found\":" << statistics["not_found"]
       << ",\"mappings_emitted\":" << emitted.size()
       << "},\"validation\":{\"has_blocking_errors\":" << (validationErrors > 0 ? "true" : "false")
       << ",\"error_count\":" << validationErrors << ",\"warning_count\":" << validationWarnings
       << "},\"mappings\":[";
  for (std::size_t i = 0; i < emitted.size(); ++i) {
    if (i) json << ',';
    const auto& m = *emitted[i];
    json << "{\"v500_unit\":\"" << escape(m.left->name) << "\",\"v500_normalized_unit\":\"" << escape(lower(m.left->name))
         << "\",\"v500_source\":" << (m.left->sourceFile.empty() ? "null" : "\"" + escape(m.left->sourceFile.generic_string()) + "\"")
         << ",\"v600_unit\":" << (m.right ? "\"" + escape(m.right->name) + "\"" : "null")
         << ",\"v600_normalized_unit\":" << (m.right ? "\"" + escape(lower(m.right->name)) + "\"" : "null")
         << ",\"v600_source\":" << (!m.right || m.right->sourceFile.empty() ? "null" : "\"" + escape(m.right->sourceFile.generic_string()) + "\"")
         << ",\"mapping_type\":\"" << m.type << "\",\"compatibility\":\"" << m.compatibility
         << "\",\"confidence\":\"" << m.confidence << "\",\"automatic_action\":\"" << m.action
         << "\",\"evidence\":{\"name_match_score\":" << m.nameScore << ",\"export_match_count\":" << m.matched.size()
         << ",\"export_missing_count\":" << m.missing.size() << ",\"signature_match_count\":" << m.signatureMatches
         << ",\"signature_incompatible_count\":" << m.signatureIncompatible << ",\"package_relation\":null},\"matched_exports\":[";
    if (options.includeSymbolDetails) for (std::size_t n = 0; n < m.matched.size(); ++n) { if (n) json << ','; json << '"' << escape(m.matched[n]) << '"'; }
    json << "],\"missing_exports\":[";
    if (options.includeSymbolDetails) for (std::size_t n = 0; n < m.missing.size(); ++n) { if (n) json << ','; json << '"' << escape(m.missing[n]) << '"'; }
    json << "],\"incompatible_exports\":[";
    if (options.includeSymbolDetails) for (std::size_t n = 0; n < m.incompatible.size(); ++n) { if (n) json << ','; json << '"' << escape(m.incompatible[n]) << '"'; }
    json << "],\"candidates\":[";
    for (std::size_t n = 0; n < m.candidates.size(); ++n) { if (n) json << ','; json << '"' << escape(m.candidates[n]) << '"'; }
    json << "],\"notes\":" << (m.notes.empty() ? "null" : "\"" + escape(m.notes) + "\"") << ",\"diagnostics\":[";
    for (std::size_t n = 0; n < m.diagnostics.size(); ++n) {
      if (n) json << ',';
      json << "{\"code\":\"" << escape(m.diagnostics[n]) << "\",\"severity\":\""
           << diagnosticSeverity(m.diagnostics[n]) << "\",\"message\":\""
           << escape(m.diagnostics[n]) << "\"}";
    }
    json << "]}";
  }
  json << "],\"unmapped_units\":[";
  bool firstUnmapped = true;
  for (const auto& m : mappings) if (m.compatibility == "not_found") { if (!firstUnmapped) json << ','; firstUnmapped = false; json << '"' << escape(m.left->name) << '"'; }
  json << "],\"ambiguous_units\":[";
  bool firstAmbiguous = true;
  for (const auto& m : mappings) if (m.type == "ambiguous") { if (!firstAmbiguous) json << ','; firstAmbiguous = false; json << '"' << escape(m.left->name) << '"'; }
  json << "],\"semantic_migration_required\":[";
  bool firstSemantic = true;
  for (const auto& m : mappings) if (m.compatibility == "semantic_migration_required" || m.compatibility == "relocated_symbols") {
    if (!firstSemantic) json << ',';
    firstSemantic = false;
    json << "{\"symbol_or_unit\":\"" << escape(m.left->name) << "\",\"reason\":\""
         << (m.compatibility == "relocated_symbols" ? "Symbols are distributed across V600 APIs." : "API flow requires semantic migration.") << "\"}";
  }
  const std::vector<std::pair<std::string, std::string>> requiredSemantic = {
      {"CSMail", "Email/DMS/UI flow requires semantic migration."},
      {"RNote", "Note API symbols require symbol-by-symbol migration."},
      {"UVariStd", "Legacy globals require semantic migration."},
      {"IAcquisti", "Legacy purchasing API requires semantic migration."},
      {"IQuoArt", "Legacy quotation API requires semantic migration."},
      {"IContatti", "Legacy contacts API requires semantic migration."},
      {"UStaMain", "Legacy print flow requires semantic migration."},
      {"CBFrig", "Legacy symbol or unit requires semantic migration."},
      {"FSecStdNS", "Legacy form API requires semantic migration."},
      {"FGridStdNS", "Legacy grid API requires semantic migration."},
      {"CercaPrezzoStd", "Legacy pricing API requires semantic migration."}};
  for (const auto& [name, reason] : requiredSemantic) {
    const auto exists = std::any_of(mappings.begin(), mappings.end(), [&](const Mapping& mapping) {
      return lower(mapping.left->name) == lower(name) &&
             (mapping.compatibility == "semantic_migration_required" || mapping.compatibility == "relocated_symbols");
    });
    if (exists) continue;
    if (!firstSemantic) json << ',';
    firstSemantic = false;
    json << "{\"symbol_or_unit\":\"" << name << "\",\"reason\":\"" << reason << "\"}";
  }
  json << "],\"diagnostics\":[";
  for (std::size_t i = 0; i < validationDiagnostics.size(); ++i) {
    if (i) json << ',';
    const auto& diagnostic = validationDiagnostics[i];
    json << "{\"code\":\"" << escape(diagnostic.code) << "\",\"severity\":\""
         << escape(diagnostic.severity) << "\",\"message\":\"" << escape(diagnostic.message) << "\"";
    if (!diagnostic.file.empty()) json << ",\"file\":\"" << escape(diagnostic.file.generic_string()) << "\"";
    if (!diagnostic.unit.empty()) json << ",\"unit\":\"" << escape(diagnostic.unit) << "\"";
    json << '}';
  }
  json << "],\"blocking_errors\":[";
  bool firstBlocking = true;
  for (const auto& diagnostic : validationDiagnostics) if (diagnostic.severity == "error") {
    if (!firstBlocking) json << ',';
    firstBlocking = false;
    json << "{\"code\":\"" << escape(diagnostic.code) << "\",\"message\":\""
         << escape(diagnostic.message) << "\"";
    if (!diagnostic.unit.empty()) json << ",\"unit\":\"" << escape(diagnostic.unit) << "\"";
    json << '}';
  }
  json << "]}";
  std::ostringstream csv;
  csv << "v500_unit,v600_unit,mapping_type,compatibility,confidence,automatic_action,matched_export_count,missing_export_count,notes\n";
  for (const auto* item : emitted) csv << csvEscape(item->left->name) << ',' << csvEscape(item->right ? item->right->name : "")
      << ',' << csvEscape(item->type) << ',' << csvEscape(item->compatibility) << ',' << csvEscape(item->confidence)
      << ',' << csvEscape(item->action) << ',' << item->matched.size() << ',' << item->missing.size() << ',' << csvEscape(item->notes) << '\n';
  std::ostringstream html;
  html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>Delphi unit mapping</title>"
          "<style>body{font-family:system-ui;margin:2rem}input{padding:.6rem;width:24rem}table{border-collapse:collapse;width:100%;margin-top:1rem}th,td{border:1px solid #ccc;padding:.5rem;text-align:left}</style></head><body>"
          "<h1>Delphi V500 to V600 unit mapping</h1><input id=\"q\" placeholder=\"Filter mappings\"><table id=\"m\"><thead><tr>"
          "<th>V500 unit</th><th>V600 unit</th><th>Type</th><th>Compatibility</th><th>Confidence</th><th>Action</th></tr></thead><tbody>";
  for (const auto* item : emitted) html << "<tr><td>" << htmlEscape(item->left->name) << "</td><td>" << htmlEscape(item->right ? item->right->name : "")
      << "</td><td>" << htmlEscape(item->type) << "</td><td>" << htmlEscape(item->compatibility) << "</td><td>" << htmlEscape(item->confidence)
      << "</td><td>" << htmlEscape(item->action) << "</td></tr>";
  html << "</tbody></table><script>q.oninput=()=>{for(const r of m.tBodies[0].rows)r.hidden=!r.innerText.toLowerCase().includes(q.value.toLowerCase())}</script></body></html>";
  return {json.str(), csv.str(), html.str(), validationErrors > 0};
}

}  // namespace jdelphiast
