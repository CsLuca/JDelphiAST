#include "jdelphiast/queries.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
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
    else if (c < 0x20) result += '?';
    else result += static_cast<char>(c);
  }
  return result;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string simpleName(std::string_view name) {
  const auto dot = name.find_last_of('.');
  return std::string(name.substr(dot == std::string_view::npos ? 0 : dot + 1));
}

std::string declarationSignature(const AstDeclaration& declaration) {
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

std::string sourceSlice(const std::string& source, SourceRange range) {
  if (range.begin.offset >= source.size() || range.end.offset > source.size() || range.end.offset < range.begin.offset)
    return {};
  return source.substr(range.begin.offset, range.end.offset - range.begin.offset);
}

const IndexedUnit* findUnit(const std::vector<IndexedUnit>& units, std::string_view name) {
  const auto key = lower(std::string(name));
  const auto found = std::find_if(units.begin(), units.end(), [&](const IndexedUnit& unit) {
    return lower(unit.name) == key;
  });
  return found == units.end() ? nullptr : &*found;
}

bool unitDeclaresSymbol(const IndexedUnit& unit, std::string_view symbol) {
  const auto key = lower(std::string(symbol));
  return std::any_of(unit.symbols.begin(), unit.symbols.end(), [&](const std::string& name) {
    return lower(name) == key;
  }) || std::any_of(unit.declarations.begin(), unit.declarations.end(), [&](const AstDeclaration& declaration) {
    return lower(simpleName(declaration.name)) == key;
  });
}

bool sourceDeclaresType(const IndexedUnit& unit, std::string_view symbol) {
  if (unit.sourceFile.empty() || !std::filesystem::is_regular_file(unit.sourceFile)) return false;
  try {
    const auto source = readFile(unit.sourceFile);
    const auto ast = parseUnit(unit.sourceFile, source);
    const bool parsed = std::any_of(ast.exports.begin(), ast.exports.end(), [&](const SymbolDeclaration& declaration) {
      return lower(declaration.name) == lower(std::string(symbol)) && lower(declaration.kind) == "class";
    }) || std::any_of(ast.declarations.begin(), ast.declarations.end(), [&](const AstDeclaration& declaration) {
      return lower(simpleName(declaration.name)) == lower(std::string(symbol)) && lower(declaration.kind) == "class";
    });
    if (parsed) return true;
    const std::regex declaration("\\b" + std::string(symbol) + "\\s*=\\s*class\\b", std::regex::icase);
    return std::regex_search(source, declaration);
  } catch (...) { return false; }
}

std::vector<std::string> activeSourceEvidence(const std::vector<IndexedUnit>& units,
                                              std::string_view symbol,
                                              std::size_t limit = 8) {
  std::vector<std::string> result;
  const auto key = lower(std::string(symbol));
  for (const auto& unit : units) {
    if (unit.sourceFile.empty() || !std::filesystem::is_regular_file(unit.sourceFile)) continue;
    try {
      const auto source = readFile(unit.sourceFile);
      if (lower(source).find(key) == std::string::npos) continue;
      const auto ast = parseUnit(unit.sourceFile, source);
      const bool found = unitDeclaresSymbol(unit, symbol) ||
          std::any_of(ast.references.begin(), ast.references.end(), [&](const SymbolReference& reference) {
            return lower(simpleName(reference.name)) == key;
          });
      if (found) result.push_back(unit.sourceFile.generic_string());
      if (result.size() >= limit) break;
    } catch (...) {}
  }
  return result;
}

std::string inferIdentifierType(const UnitAst& ast, std::string expression) {
  const auto dot = expression.find('.');
  if (dot != std::string::npos) expression.resize(dot);
  const auto key = lower(expression);
  for (const auto& declaration : ast.declarations) {
    if (lower(simpleName(declaration.name)) == key && !declaration.type.empty()) return declaration.type;
    for (const auto& parameter : declaration.parameters)
      if (lower(parameter.name) == key) return parameter.type;
  }
  return {};
}

bool typeCompatible(std::string_view actual, std::string_view expected) {
  if (actual.empty() || expected.empty()) return false;
  const auto a = lower(std::string(actual));
  const auto e = lower(std::string(expected));
  if (a == e) return true;
  const std::set<std::string> stringTypes = {"string", "ansistring", "unicodestring", "widestring"};
  const std::set<std::string> integerTypes = {"integer", "longint", "smallint", "word", "cardinal", "int64"};
  return (stringTypes.contains(a) && stringTypes.contains(e)) ||
         (integerTypes.contains(a) && integerTypes.contains(e));
}

std::string mappingStatus(const std::filesystem::path& catalog, std::string_view unit) {
  if (catalog.empty()) return "unknown";
  try {
    for (const auto& mapping : loadUnitMappingCatalog(catalog)) {
      if (lower(mapping.sourceUnit) != lower(std::string(unit))) continue;
      if (mapping.mappingType == "removed_no_equivalent" || mapping.mappingType == "relocated_symbols" ||
          mapping.mappingType == "semantic_migration_required") return mapping.mappingType;
      return "unknown";
    }
  } catch (...) {}
  return "unknown";
}

std::string usageFor(const UnitAst& ast, const SymbolReference& reference) {
  if (std::any_of(ast.calls.begin(), ast.calls.end(), [&](const AstCall& call) {
        return reference.range.begin.offset >= call.range.begin.offset && reference.range.begin.offset < call.range.end.offset;
      })) return "invocation";
  if (std::any_of(ast.assignments.begin(), ast.assignments.end(), [&](const AstAssignment& assignment) {
        return reference.range.begin.offset >= assignment.range.begin.offset &&
               reference.range.end.offset <= assignment.range.end.offset && reference.name == assignment.left;
      })) return "write";
  return "type_reference";
}

bool genericLegacySymbol(std::string_view symbol) {
  static const std::unordered_set<std::string> generic = {
      "add", "create", "destroy", "format", "indexof", "fieldbyname", "open", "close",
      "free", "freeandnil", "assigned", "inttostr", "vartostr"};
  return generic.contains(lower(std::string(symbol)));
}

std::string businessClassification(std::string_view symbol) {
  const auto key = lower(std::string(symbol));
  static const std::unordered_set<std::string> business = {
      "cercaprezzostd", "fsecstdns", "fgridstdns", "csmail", "iacquisti", "iquoart",
      "icontatti", "ustamain", "cbfrig"};
  return business.contains(key) ? "business_api_manual_required" : std::string{};
}

void writeNullable(std::ostringstream& output, std::string_view value) {
  if (value.empty()) output << "null"; else output << '"' << escape(value) << '"';
}

}  // namespace

std::string compilerLogJson(const std::filesystem::path& input) {
  const auto source = readFile(input);
  struct Error { std::string file, column, severity, code, message, symbol; std::size_t line{}; };
  std::vector<Error> errors;
  const std::regex prefixed(
      R"(^\s*\[dcc32\s+(Error|Fatal|Warning)\]\s+(.+?\.pas)\((\d+)(?:,(\d+))?\)\s*:\s*([EFW]\d{4})\s*:?[ \t]*(.*)$)",
      std::regex::icase);
  const std::regex plain(
      R"(^\s*(.+?\.pas)\((\d+)(?:,(\d+))?\)\s+(Error|Fatal|Warning)\s*:?\s*([EFW]\d{4})\s*:?[ \t]*(.*)$)",
      std::regex::icase);
  std::istringstream lines(source);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch item;
    Error error;
    if (std::regex_match(line, item, prefixed)) {
      error = {item[2].str(), item[4].str(), item[1].str(), item[5].str(), item[6].str(), {},
               static_cast<std::size_t>(std::stoull(item[3].str()))};
    } else if (std::regex_match(line, item, plain)) {
      error = {item[1].str(), item[3].str(), item[4].str(), item[5].str(), item[6].str(), {},
               static_cast<std::size_t>(std::stoull(item[2].str()))};
    } else continue;
    const std::regex quoted(R"('([^']+)')");
    std::smatch symbol;
    if (lower(error.message).find("undeclared identifier") != std::string::npos &&
        std::regex_search(error.message, symbol, quoted)) error.symbol = symbol[1].str();
    errors.push_back(std::move(error));
  }
  std::ostringstream output;
  output << "{\"schema_version\":\"2.2\",\"errors\":[";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i) output << ',';
    const auto& error = errors[i];
    output << "{\"ordinal\":" << i + 1 << ",\"file\":\"" << escape(error.file)
           << "\",\"line\":" << error.line << ",\"column\":";
    if (error.column.empty()) output << "null"; else output << error.column;
    output << ",\"severity\":\"" << escape(error.severity) << "\",\"code\":\""
           << escape(error.code) << "\",\"message\":\"" << escape(error.message) << "\",\"symbol\":";
    writeNullable(output, error.symbol);
    output << '}';
  }
  output << "],\"first_actionable_error\":";
  if (errors.empty()) output << "null";
  else output << "{\"ordinal\":1,\"file\":\"" << escape(errors.front().file)
              << "\",\"line\":" << errors.front().line << ",\"code\":\"" << escape(errors.front().code) << "\"}";
  struct Group { std::string file, code; std::vector<std::size_t> lines; };
  std::vector<Group> groups;
  std::unordered_map<std::string, std::size_t> groupByKey;
  for (const auto& error : errors) {
    const auto key = lower(error.file) + "\n" + lower(error.code);
    const auto [position, inserted] = groupByKey.emplace(key, groups.size());
    if (inserted) groups.push_back({error.file, error.code, {}});
    groups[position->second].lines.push_back(error.line);
  }
  output << ",\"error_groups\":[";
  for (std::size_t i = 0; i < groups.size(); ++i) {
    if (i) output << ',';
    output << "{\"file\":\"" << escape(groups[i].file) << "\",\"code\":\"" << escape(groups[i].code)
           << "\",\"count\":" << groups[i].lines.size() << ",\"lines\":[";
    for (std::size_t n = 0; n < groups[i].lines.size(); ++n) {
      if (n) output << ',';
      output << groups[i].lines[n];
    }
    output << "]}";
  }
  output << "]}";
  return output.str();
}

std::string legacyReferencesJson(const std::filesystem::path& file, std::string_view legacyUnit,
                                 const std::vector<IndexedUnit>& left, const std::vector<IndexedUnit>& right,
                                 const std::filesystem::path& unitMap) {
  const auto source = readFile(file);
  const auto ast = parseUnit(file, source);
  const auto legacy = findUnit(left, legacyUnit);
  const auto unitStatus = mappingStatus(unitMap, legacyUnit);
  const bool importsLegacy = std::any_of(ast.uses.begin(), ast.uses.end(), [&](const UsesItem& item) {
    return lower(item.name) == lower(std::string(legacyUnit));
  });
  std::unordered_set<std::string> exports;
  if (legacy) for (const auto& symbol : legacy->symbols) exports.insert(lower(symbol));
  struct Ref { std::string symbol, kind, usage, status; SourceRange range; std::vector<std::string> candidates; };
  std::vector<Ref> references;
  std::vector<Ref> unresolvedReferences;
  std::size_t ignoredGenericReferences = 0;
  for (const auto& reference : ast.references) {
    const auto simple = simpleName(reference.name);
    const bool exactLegacyOwner = legacy && unitDeclaresSymbol(*legacy, simple);
    const auto call = std::find_if(ast.calls.begin(), ast.calls.end(), [&](const AstCall& candidate) {
      return reference.range.begin.offset >= candidate.range.begin.offset && reference.range.begin.offset < candidate.range.end.offset &&
             lower(simpleName(candidate.name)) == lower(simple);
    });
    const bool qualifiedLegacy = reference.name.find('.') != std::string::npos &&
        lower(reference.name.substr(0, reference.name.find('.'))) == lower(std::string(legacyUnit));
    if (!exactLegacyOwner && !qualifiedLegacy && call == ast.calls.end()) continue;
    if (!exactLegacyOwner && !qualifiedLegacy && genericLegacySymbol(simple)) {
      ++ignoredGenericReferences;
      continue;
    }
    Ref item{simple, "unknown", usageFor(ast, reference), exactLegacyOwner ? "legacy_symbol_unmapped" : "ambiguous",
             reference.range, {}};
    if (legacy) for (const auto& declaration : legacy->declarations)
      if (lower(simpleName(declaration.name)) == lower(simple)) { item.kind = declaration.kind; break; }
    if (exactLegacyOwner) for (const auto& unit : right)
      if (unitDeclaresSymbol(unit, simple)) item.candidates.push_back(unit.name);
    const auto sourceEvidence = item.candidates.empty() ? activeSourceEvidence(right, simple) : std::vector<std::string>{};
    if (exactLegacyOwner && item.candidates.size() == 1 && unitStatus == "relocated_symbols")
      item.status = "relocated_symbols";
    else if (!item.candidates.empty() || !sourceEvidence.empty()) item.status = "ambiguous";
    if (exactLegacyOwner) references.push_back(std::move(item));
    else unresolvedReferences.push_back(std::move(item));
  }
  if (unresolvedReferences.empty() && importsLegacy && (!legacy || !legacy->complete) &&
      (unitStatus == "relocated_symbols" || unitStatus == "semantic_migration_required")) {
    for (const auto& call : ast.calls) {
      const auto member = simpleName(call.name);
      if (genericLegacySymbol(member)) { ++ignoredGenericReferences; continue; }
      bool knownElsewhere = false;
      for (const auto& unit : left)
        if (lower(unit.name) != lower(std::string(legacyUnit)) && unitDeclaresSymbol(unit, member)) {
          knownElsewhere = true;
          break;
        }
      if (knownElsewhere) continue;
      Ref item{member, "unknown", "invocation", "ambiguous", call.range, {}};
      for (const auto& unit : right) if (unitDeclaresSymbol(unit, member)) item.candidates.push_back(unit.name);
      if (item.candidates.size() == 1) item.status = "relocated_symbols";
      unresolvedReferences.push_back(std::move(item));
    }
  }
  std::sort(unresolvedReferences.begin(), unresolvedReferences.end(), [](const Ref& a, const Ref& b) {
    return a.range.begin.offset < b.range.begin.offset;
  });
  std::ostringstream output;
  output << "{\"schema_version\":\"2.2\",\"file\":\"" << escape(file.filename().string())
         << "\",\"legacy_unit\":\"" << escape(legacyUnit) << "\",\"legacy_unit_status\":\""
         << escape(unitStatus) << "\",\"references\":[";
  for (std::size_t i = 0; i < references.size(); ++i) {
    if (i) output << ',';
    const auto& reference = references[i];
    output << "{\"symbol\":\"" << escape(reference.symbol) << "\",\"kind\":\"" << escape(reference.kind)
           << "\",\"line\":" << reference.range.begin.line << ",\"column\":" << reference.range.begin.column
           << ",\"usage\":\"" << reference.usage << "\",\"v500_owner\":\"" << escape(legacyUnit)
           << "\",\"v600_candidates\":[";
    for (std::size_t c = 0; c < reference.candidates.size(); ++c) {
      if (c) output << ',';
      output << '"' << escape(reference.candidates[c]) << '"';
    }
    output << "],\"mapping_status\":\"" << reference.status
            << "\",\"automatic_action\":\"manual_required\"}";
  }
  output << "],\"unresolved_active_references\":[";
  for (std::size_t i = 0; i < unresolvedReferences.size(); ++i) {
    if (i) output << ',';
    const auto& reference = unresolvedReferences[i];
    output << "{\"symbol\":\"" << escape(reference.symbol) << "\",\"line\":" << reference.range.begin.line
           << ",\"column\":" << reference.range.begin.column << ",\"usage\":\"" << reference.usage
           << "\",\"possible_v500_owner\":";
    if (reference.status == "ambiguous") output << "null"; else output << '"' << escape(legacyUnit) << '"';
    output << ",\"mapping_status\":\"" << reference.status << "\",\"v600_candidates\":[";
    for (std::size_t c = 0; c < reference.candidates.size(); ++c) {
      if (c) output << ',';
      output << '"' << escape(reference.candidates[c]) << '"';
    }
    output << "]}";
  }
  const bool removalAllowed = references.empty() && unresolvedReferences.empty() && !importsLegacy;
  output << "],\"ignored_generic_references_count\":" << ignoredGenericReferences
         << ",\"removal\":{\"allowed\":" << (removalAllowed ? "true" : "false") << ",\"reason\":";
  const auto* blocker = !references.empty() ? &references.front() : !unresolvedReferences.empty() ? &unresolvedReferences.front() : nullptr;
  if (!blocker && importsLegacy) output << "\"legacy_unit_exports_incomplete\"";
  else if (!blocker) output << "null";
  else output << '"' << escape(blocker->status + ":" + blocker->symbol) << '"';
  output << "}}";
  return output.str();
}

std::string modelMigrationJson(const std::filesystem::path& file, std::string_view symbol,
                               const std::vector<IndexedUnit>& left, const std::vector<IndexedUnit>& right,
                               const std::filesystem::path&) {
  const auto source = readFile(file);
  const auto ast = parseUnit(file, source);
  const IndexedUnit* oldOwner = nullptr;
  const IndexedUnit* newOwner = nullptr;
  for (const auto& used : ast.uses) {
    const auto* unit = findUnit(left, used.name);
    if (unit && (unitDeclaresSymbol(*unit, symbol) || sourceDeclaresType(*unit, symbol))) {
      oldOwner = unit;
      break;
    }
  }
  for (const auto& unit : left)
    if (!oldOwner && unitDeclaresSymbol(unit, symbol)) { oldOwner = &unit; break; }
  for (const auto& unit : right)
    if (unitDeclaresSymbol(unit, symbol)) { newOwner = &unit; break; }
  const auto probableUnit = lower(std::string(symbol).starts_with("T") ? std::string(symbol).substr(1) : std::string(symbol));
  for (const auto& unit : left)
    if (!oldOwner && lower(simpleName(unit.name)) == probableUnit && sourceDeclaresType(unit, symbol)) { oldOwner = &unit; break; }
  for (const auto& unit : right)
    if (!newOwner && lower(simpleName(unit.name)) == probableUnit && sourceDeclaresType(unit, symbol)) { newOwner = &unit; break; }
  static const std::vector<std::string> modelMembers = {
      "EnableOnChange", "FieldValues", "GetCsField", "CSSeek", "CSModify", "CSResetCampi"};
  std::vector<std::string> used;
  for (const auto& member : modelMembers) {
    const std::regex token("\\b" + member + "\\b", std::regex::icase);
    if (std::regex_search(source, token)) used.push_back(member);
  }
  std::vector<std::string> compatible, missing;
  for (const auto& member : used) {
    const bool exists = newOwner && (std::any_of(newOwner->symbols.begin(), newOwner->symbols.end(), [&](const std::string& name) {
      return lower(name) == lower(member);
    }) || std::any_of(newOwner->declarations.begin(), newOwner->declarations.end(), [&](const AstDeclaration& declaration) {
      return lower(simpleName(declaration.name)) == lower(member);
    }) || (!newOwner->sourceFile.empty() && std::filesystem::is_regular_file(newOwner->sourceFile) && [&] {
      try {
        const auto targetAst = parseUnit(newOwner->sourceFile, readFile(newOwner->sourceFile));
        return std::any_of(targetAst.declarations.begin(), targetAst.declarations.end(), [&](const AstDeclaration& declaration) {
          return lower(simpleName(declaration.name)) == lower(member) &&
                 (declaration.ownerType.empty() || lower(declaration.ownerType) == lower(std::string(symbol)));
        });
      } catch (...) { return false; }
    }()));
    (exists ? compatible : missing).push_back(member);
  }
  const auto writeArray = [](std::ostringstream& out, const std::vector<std::string>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) { if (i) out << ','; out << '"' << escape(values[i]) << '"'; }
  };
  std::ostringstream output;
  output << "{\"schema_version\":\"2.2\",\"model\":\"" << escape(symbol) << "\",\"v500\":{\"unit\":";
  writeNullable(output, oldOwner ? oldOwner->name : "");
  output << ",\"source_file\":";
  writeNullable(output, oldOwner ? oldOwner->sourceFile.generic_string() : "");
  output << ",\"members_used\":["; writeArray(output, used);
  output << "]},\"v600\":{\"type_exists\":" << (newOwner ? "true" : "false") << ",\"unit\":";
  writeNullable(output, newOwner ? newOwner->name : "");
  output << ",\"source_file\":";
  writeNullable(output, newOwner ? newOwner->sourceFile.generic_string() : "");
  output << ",\"compatible_members\":["; writeArray(output, compatible);
  output << "],\"missing_or_unresolved_members\":["; writeArray(output, missing);
  output << "]},\"classification\":\"legacy_model_migration_required\",\"automatic_action\":\"manual_required\"}";
  return output.str();
}

std::string diagnoseJson(const DiagnoseOptions& options, const std::vector<IndexedUnit>& right,
                         const std::vector<IndexedUnit>& left) {
  const auto source = readFile(options.file);
  const auto ast = parseUnit(options.file, source);
  const AstCall* selected = nullptr;
  std::size_t selectedDepth = 0;
  int selectedEvidence = -1;
  bool callSelectionAmbiguous = false;
  for (const auto& call : ast.calls) {
    if (call.range.begin.line > options.line || call.range.end.line < options.line) continue;
    if (!options.symbol.empty() && lower(simpleName(call.name)) != lower(options.symbol)) continue;
    std::size_t depth = 0;
    for (const auto& parent : ast.calls)
      if (&parent != &call && parent.range.begin.offset <= call.range.begin.offset &&
          parent.range.end.offset >= call.range.end.offset) ++depth;
    int evidence = static_cast<int>(depth) * 100;
    bool hasArityMatch = false;
    bool hasTypedMismatch = false;
    for (const auto& unit : right) for (const auto& declaration : unit.declarations) {
      if (lower(simpleName(declaration.name)) != lower(simpleName(call.name))) continue;
      if (declaration.parameters.size() != call.arguments.size()) continue;
      hasArityMatch = true;
      for (std::size_t i = 0; i < call.arguments.size(); ++i) {
        auto actual = call.arguments[i].resolvedType;
        if (actual.empty()) actual = inferIdentifierType(ast, call.arguments[i].text);
        if (!actual.empty() && !declaration.parameters[i].type.empty() &&
            !typeCompatible(actual, declaration.parameters[i].type)) hasTypedMismatch = true;
      }
    }
    if (hasArityMatch) evidence += 10;
    if (options.errorCode == "E2010" && hasTypedMismatch) evidence += 20;
    if (!selected || evidence > selectedEvidence) {
      selected = &call;
      selectedDepth = depth;
      selectedEvidence = evidence;
      callSelectionAmbiguous = false;
    } else if (evidence == selectedEvidence && depth == selectedDepth &&
               call.range.begin.offset != selected->range.begin.offset) {
      callSelectionAmbiguous = true;
    }
  }
  std::optional<AstCall> implicitCall;
  if (!selected) for (const auto& assignment : ast.assignments) {
    if (assignment.range.begin.line > options.line || assignment.range.end.line < options.line) continue;
    auto expression = assignment.right;
    while (!expression.empty() && std::isspace(static_cast<unsigned char>(expression.back()))) expression.pop_back();
    const std::regex bareCall(R"(^\s*([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)+)\s*$)");
    std::smatch match;
    if (!std::regex_match(expression, match, bareCall)) continue;
    implicitCall.emplace();
    implicitCall->name = match[1].str();
    implicitCall->range = assignment.range;
    implicitCall->range.begin.offset = assignment.range.end.offset - expression.size();
    implicitCall->range.begin.line = assignment.range.end.line;
    implicitCall->range.begin.column = assignment.range.end.column > expression.size()
        ? assignment.range.end.column - expression.size() : 1;
    implicitCall->assignmentTarget = {
        assignment.left.find('.') == std::string::npos ? "identifier" : "property",
        assignment.left, assignment.targetType};
    selected = &*implicitCall;
    selectedDepth = 0;
    selectedEvidence = 0;
    callSelectionAmbiguous = false;
    break;
  }
  const std::string requested = !options.symbol.empty() ? options.symbol : selected ? simpleName(selected->name) : std::string{};
  const auto business = businessClassification(requested);
  const auto mappings = [&] {
    try { return options.unitMap.empty() ? std::vector<UnitMappingSuggestion>{} : loadUnitMappingCatalog(options.unitMap); }
    catch (...) { return std::vector<UnitMappingSuggestion>{}; }
  }();
  std::unordered_set<std::string> mappedTargetUnits;
  for (const auto& mapping : mappings)
    if (!mapping.targetUnit.empty()) mappedTargetUnits.insert(lower(mapping.targetUnit));
  struct Candidate { const IndexedUnit* unit; const AstDeclaration* declaration; int score; std::string compatibility; };
  std::vector<Candidate> candidates;
  std::set<std::string> uses;
  for (const auto& used : ast.uses) uses.insert(lower(used.name));
  const std::string receiverExpression = selected && selected->name.find('.') != std::string::npos
      ? selected->name.substr(0, selected->name.rfind('.')) : std::string{};
  const auto sourceMember = selected ? simpleName(selected->name) : requested;
  auto receiverType = inferIdentifierType(ast, receiverExpression);
  if (receiverType.empty() && !receiverExpression.empty()) {
    const auto receiverName = simpleName(receiverExpression);
    const auto classReceiver = std::any_of(right.begin(), right.end(), [&](const IndexedUnit& unit) {
      return std::any_of(unit.declarations.begin(), unit.declarations.end(), [&](const AstDeclaration& declaration) {
        return lower(declaration.ownerType) == lower(receiverName);
      });
    });
    if (classReceiver) receiverType = receiverName;
  }
  for (const auto& unit : right) for (const auto& declaration : unit.declarations) {
    if (lower(simpleName(declaration.name)) != lower(sourceMember)) continue;
    int score = 0;
    const auto qualifier = lower(receiverExpression);
    const auto qualifierSimple = lower(simpleName(receiverExpression));
    const bool ownerMatch = !qualifier.empty() && !declaration.ownerType.empty() &&
        lower(declaration.ownerType) == qualifierSimple;
    const bool unitMatch = !qualifier.empty() &&
        (lower(unit.name) == qualifier || lower(simpleName(unit.name)) == qualifierSimple);
    if (!qualifier.empty() && !declaration.ownerType.empty() && !ownerMatch && !unitMatch &&
        !receiverType.empty() && lower(receiverType) != lower(declaration.ownerType)) continue;
    if (uses.contains(lower(unit.name))) score += 20;
    if (mappedTargetUnits.contains(lower(unit.name))) score += 10;
    if (ownerMatch || unitMatch) score += 35;
    if (!receiverType.empty() && !declaration.ownerType.empty()) {
      if (lower(receiverType) == lower(declaration.ownerType)) score += 20;
    }
    if (!unit.packageName.empty() && !options.dproj.empty()) {
      const auto projectText = lower(readFile(options.dproj));
      if (projectText.find(lower(unit.packageName)) != std::string::npos ||
          projectText.find(lower(unit.packageDcp)) != std::string::npos) score += 10;
    }
    std::string compatibility = "unknown";
    if (selected && declaration.parameters.size() == selected->arguments.size()) {
      score += 20;
      bool known = true, compatible = true;
      for (std::size_t i = 0; i < selected->arguments.size(); ++i) {
        auto actual = selected->arguments[i].resolvedType;
        if (actual.empty()) actual = inferIdentifierType(ast, selected->arguments[i].text);
        if (actual.empty() || declaration.parameters[i].type.empty()) known = false;
        else if (!typeCompatible(actual, declaration.parameters[i].type)) compatible = false;
      }
      compatibility = !known ? "unknown" : compatible ? "compatible" : "incompatible";
      if (compatible && known) score += 10;
    }
    if (!declaration.signature.empty()) score += 5;
    candidates.push_back({&unit, &declaration, std::min(score, 100), compatibility});
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    return a.score > b.score;
  });
  if (selected && std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
        return candidate.declaration->parameters.size() == selected->arguments.size();
      })) {
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
      return candidate.declaration->parameters.size() != selected->arguments.size();
    }), candidates.end());
  }
  const auto topScore = candidates.empty() ? 0 : candidates.front().score;
  candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
    return candidate.score < topScore;
  }), candidates.end());
  const Candidate* selectedCandidate = candidates.empty() ||
      (candidates.size() > 1 && candidates[0].score == candidates[1].score) ? nullptr : &candidates.front();
  if (selectedCandidate && selected && !selected->arguments.empty()) {
    const auto actualFirst = selected->arguments.front().resolvedType.empty()
        ? inferIdentifierType(ast, selected->arguments.front().text) : selected->arguments.front().resolvedType;
    bool acceptsCurrent = false, acceptsConnection = false;
    for (const auto& candidate : candidates) {
      if (candidate.score != candidates.front().score) continue;
      if (candidate.declaration->parameters.empty()) continue;
      const auto expected = lower(candidate.declaration->parameters.front().type);
      acceptsCurrent = acceptsCurrent || typeCompatible(actualFirst, expected);
      acceptsConnection = acceptsConnection || (lower(actualFirst) == "taziendastd" &&
          (expected == "tcsedatabase" || expected == "ierpconnection"));
    }
    if (acceptsCurrent && acceptsConnection) selectedCandidate = nullptr;
  }
  std::string classification = "unresolved", confidence = "low", action = "review_required";
  const auto mapped = std::find_if(mappings.begin(), mappings.end(), [&](const UnitMappingSuggestion& mapping) {
    return lower(mapping.sourceUnit) == lower(requested);
  });
  bool v600OwnerAmbiguous = false;
  if (options.errorCode == "F2613") {
    if (mapped != mappings.end() && !mapped->targetUnit.empty()) {
      classification = mapped->mappingType == "namespace_migration" ? "namespace_mapping" : "unit_mapping";
      confidence = mapped->confidence.empty() ? "high" : mapped->confidence;
      action = mapped->automaticAction == "conditional_replace_in_uses" ? "conditional_replace" : "replace_in_uses";
    } else if (const auto* unit = findUnit(right, requested)) {
      classification = unit->packageName.empty() ? "build_path_required" : "package_dependency_required";
      confidence = "high";
      action = "review_required";
    }
  } else if (!business.empty()) { classification = business; action = "manual_required"; confidence = "high"; }
  else if (options.errorCode == "E2003" && lower(requested) == "tcsfields") {
    classification = "legacy_model_migration_required"; action = "manual_required"; confidence = "high";
  }
  else if (options.errorCode == "E2037") {
    classification = "signature_mismatch"; action = "replace_signature"; confidence = candidates.empty() ? "low" : "high";
  }
  else if (options.errorCode == "E2003") {
    bool legacyFound = false, currentFound = false;
    for (const auto& unit : left) legacyFound = legacyFound || unitDeclaresSymbol(unit, requested);
    for (const auto& unit : right) currentFound = currentFound || unitDeclaresSymbol(unit, requested);
    const auto currentSourceEvidence = currentFound ? std::vector<std::string>{} : activeSourceEvidence(right, requested);
    if (!currentFound && !currentSourceEvidence.empty()) {
      v600OwnerAmbiguous = true;
      classification = "ambiguous"; action = "review_required"; confidence = "medium";
    }
    if (legacyFound && !currentFound) { classification = "legacy_symbol_unmapped"; action = "manual_required"; confidence = "high"; }
    if (legacyFound && !currentSourceEvidence.empty()) {
      v600OwnerAmbiguous = true;
      classification = "ambiguous"; action = "review_required"; confidence = "medium";
    }
  }
  else if (!selectedCandidate && !candidates.empty()) classification = "ambiguous";
  else if (selectedCandidate && selected) {
    const auto actual = selected->assignmentTarget.resolvedType;
    const auto returned = selectedCandidate->declaration->type;
    if (!actual.empty() && (returned.empty() || !typeCompatible(actual, returned))) {
      classification = "return_type_mismatch"; confidence = "high"; action = "review_required";
    } else if (selectedCandidate->compatibility == "incompatible" || options.errorCode == "E2010") {
      classification = "signature_mismatch"; confidence = "high"; action = "review_required";
    } else { classification = "unresolved"; confidence = "medium"; }
  }
  const auto api = lower(requested);
  const bool specialApi = api == "execsql" || api == "executescalar";
  if (specialApi && selected && !selected->assignmentTarget.resolvedType.empty() && selectedCandidate &&
      (selectedCandidate->declaration->type.empty() ||
       !typeCompatible(selected->assignmentTarget.resolvedType, selectedCandidate->declaration->type))) {
    classification = "return_type_mismatch"; confidence = "high"; action = "review_required";
  }
  if (callSelectionAmbiguous) {
    classification = "ambiguous";
    confidence = "low";
    action = "review_required";
    selectedCandidate = nullptr;
  }
  bool safeConnectionReplacement = false;
  if (selectedCandidate && selected) {
    for (std::size_t i = 0; i < selected->arguments.size() && i < selectedCandidate->declaration->parameters.size(); ++i) {
      auto actual = selected->arguments[i].resolvedType;
      if (actual.empty()) actual = inferIdentifierType(ast, selected->arguments[i].text);
      const auto expected = selectedCandidate->declaration->parameters[i].type;
      if (lower(actual) != "taziendastd" ||
          (lower(expected) != "tcsedatabase" && lower(expected) != "ierpconnection")) continue;
      const bool acceptsCurrent = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
        return candidate.declaration->parameters.size() == selected->arguments.size() &&
               i < candidate.declaration->parameters.size() &&
               typeCompatible(actual, candidate.declaration->parameters[i].type);
      });
      const bool connectionResolved = std::any_of(right.begin(), right.end(), [&](const IndexedUnit& unit) {
        return std::any_of(unit.declarations.begin(), unit.declarations.end(), [&](const AstDeclaration& declaration) {
          return lower(declaration.ownerType) == "taziendastd" && lower(simpleName(declaration.name)) == "connection" &&
                 typeCompatible(declaration.type, expected);
        });
      });
      if (!acceptsCurrent && connectionResolved) safeConnectionReplacement = true;
    }
  }
  if (safeConnectionReplacement) {
    classification = "signature_mismatch";
    confidence = "high";
    action = "replace_argument";
  }
  std::string diagnosisReason;
  if (classification == "business_api_manual_required")
    diagnosisReason = lower(requested) == "cercaprezzostd"
        ? "Price-list business semantics require verified replacement."
        : "Business semantics require a verified replacement.";
  else if (classification == "legacy_symbol_unmapped")
    diagnosisReason = "No active V600 replacement was verified.";
  else if (v600OwnerAmbiguous)
    diagnosisReason = "symbol_exists_in_v600_but_owner_unit_is_ambiguous";
  else if (classification == "return_type_mismatch" && specialApi)
    diagnosisReason = "Return type changes error-handling semantics.";
  std::ostringstream output;
  output << "{\"schema_version\":\"2.2\",\"diagnosis\":{\"file\":\"" << escape(options.file.filename().string())
         << "\",\"line\":" << options.line << ",\"error_code\":\"" << escape(options.errorCode)
         << "\",\"symbol\":"; writeNullable(output, options.symbol);
  output << ",\"source_expression\":"; writeNullable(output, selected ? sourceSlice(source, selected->range) : "");
  output << ",\"source_range\":";
  if (!selected) output << "null";
  else output << "{\"start_line\":" << selected->range.begin.line << ",\"start_column\":" << selected->range.begin.column
              << ",\"end_line\":" << selected->range.end.line << ",\"end_column\":" << selected->range.end.column << '}';
  output << ",\"callee\":{\"qualifier\":"; writeNullable(output, receiverExpression);
  output << ",\"member\":"; writeNullable(output, sourceMember);
  output << ",\"qualified_source_name\":"; writeNullable(output, selected ? selected->name : "");
  output << ",\"argument_count\":" << (selected ? selected->arguments.size() : 0)
         << ",\"nesting_depth\":" << selectedDepth << '}';
  output << ",\"classification\":\"" << classification << "\",\"confidence\":\"" << confidence
         << "\",\"recommended_action\":\"" << action << "\",\"reason\":";
  writeNullable(output, diagnosisReason);
  output << ",\"diagnostics\":[]},\"receiver\":{\"expression\":";
  writeNullable(output, receiverExpression); output << ",\"type\":"; writeNullable(output, receiverType); output << "},\"callee_candidates\":[";
  for (std::size_t c = 0; c < candidates.size(); ++c) {
    if (c) output << ',';
    const auto& candidate = candidates[c];
    output << "{\"qualified_name\":\"" << escape(candidate.unit->name + "." + simpleName(candidate.declaration->name))
           << "\",\"unit\":\"" << escape(candidate.unit->name) << "\",\"signature\":\""
           << escape(declarationSignature(*candidate.declaration)) << "\",\"return_type\":";
    writeNullable(output, candidate.declaration->type);
    output << ",\"parameters\":[";
    for (std::size_t p = 0; p < candidate.declaration->parameters.size(); ++p) {
      if (p) output << ',';
      output << "{\"position\":" << p + 1 << ",\"name\":\"" << escape(candidate.declaration->parameters[p].name)
             << "\",\"type\":\"" << escape(candidate.declaration->parameters[p].type) << "\"}";
    }
    output << "],\"package\":{\"name\":"; writeNullable(output, candidate.unit->packageName);
    output << ",\"dcp\":"; writeNullable(output, candidate.unit->packageDcp);
    output << ",\"bpl\":"; writeNullable(output, candidate.unit->packageBpl);
    const auto normalizedScore = topScore > 0 ? candidate.score * 100 / topScore : 0;
    output << "},\"match_score\":" << normalizedScore << ",\"argument_compatibility\":\""
           << candidate.compatibility << "\"}";
  }
  output << "],\"arguments\":[";
  if (selected) for (std::size_t i = 0; i < selected->arguments.size(); ++i) {
    if (i) output << ',';
    auto actual = selected->arguments[i].resolvedType;
    if (actual.empty()) actual = inferIdentifierType(ast, selected->arguments[i].text);
    const auto expected = selectedCandidate && i < selectedCandidate->declaration->parameters.size()
        ? selectedCandidate->declaration->parameters[i].type : std::string{};
    const bool compatible = typeCompatible(actual, expected);
    std::string suggestion;
    if (selectedCandidate && lower(actual) == "taziendastd" &&
        (lower(expected) == "tcsedatabase" || lower(expected) == "ierpconnection")) {
      if (safeConnectionReplacement) suggestion = selected->arguments[i].text + ".Connection";
    }
    output << "{\"position\":" << i + 1 << ",\"expression\":\"" << escape(selected->arguments[i].text)
           << "\",\"inferred_type\":"; writeNullable(output, actual);
    output << ",\"expected_type\":"; writeNullable(output, expected);
    output << ",\"compatible\":" << (compatible ? "true" : "false") << ",\"suggested_expression\":";
    writeNullable(output, suggestion);
    if (!suggestion.empty()) output << ",\"suggestion_confidence\":\"high\"";
    output << '}';
  }
  output << "],\"expected_type\":{\"type\":";
  writeNullable(output, selected ? selected->assignmentTarget.resolvedType : "");
  output << "},\"actual_type\":{\"type\":";
  writeNullable(output, selectedCandidate ? selectedCandidate->declaration->type : "");
  output << "},\"legacy_mapping\":{\"source_unit\":";
  writeNullable(output, mapped == mappings.end() ? "" : mapped->sourceUnit);
  output << ",\"target_unit\":"; writeNullable(output, mapped == mappings.end() ? "" : mapped->targetUnit);
  output << ",\"mapping_type\":"; writeNullable(output, mapped == mappings.end() ? "" : mapped->mappingType);
  output << "},\"references\":[";
  bool firstReference = true;
  for (const auto& reference : ast.references) {
    if (reference.range.begin.line != options.line) continue;
    if (!firstReference) output << ',';
    firstReference = false;
    output << "{\"symbol\":\"" << escape(reference.name) << "\",\"line\":" << reference.range.begin.line
           << ",\"column\":" << reference.range.begin.column << "}";
  }
  output << ']';
  if (!business.empty() || classification == "legacy_symbol_unmapped") {
    output << ",\"symbol_analysis\":{\"symbol\":\"" << escape(requested)
           << "\",\"classification\":\"" << classification
           << "\",\"automatic_action\":\"manual_required\",\"reason\":";
    writeNullable(output, diagnosisReason);
    output << '}';
  }
  if (specialApi) {
    output << ",\"api_analysis\":{\"api_name\":\"" << escape(requested)
           << "\",\"receiver_expression\":"; writeNullable(output, receiverExpression);
    output << ",\"receiver_type\":"; writeNullable(output, receiverType);
    output << ",\"resolved_signature\":";
    writeNullable(output, selectedCandidate ? declarationSignature(*selectedCandidate->declaration) : "");
    output << ",\"return_type\":"; writeNullable(output, selectedCandidate ? selectedCandidate->declaration->type : "");
    output << ",\"legacy_assignment_target\":"; writeNullable(output, selected ? selected->assignmentTarget.text : "");
    output << ",\"legacy_assignment_type\":"; writeNullable(output, selected ? selected->assignmentTarget.resolvedType : "");
    output << ",\"classification\":\"" << classification
           << "\",\"automatic_migration_safe\":false,\"reason\":\"Return type changes error-handling semantics.\"}";
  }
  output << '}';
  return output.str();
}

}  // namespace jdelphiast
