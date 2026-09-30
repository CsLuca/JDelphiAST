#include "jdelphiast/queries.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
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

std::vector<std::string> activeSourceEvidence(const std::vector<IndexedUnit>& units,
                                              std::string_view symbol,
                                              std::size_t limit = 8) {
  std::vector<std::string> result;
  const auto key = lower(std::string(symbol));
  for (const auto& unit : units) {
    const auto found = std::any_of(unit.sourceEvidence.begin(), unit.sourceEvidence.end(), [&](const auto& evidence) {
      return lower(simpleName(evidence.symbol)) == key && evidence.usage != "inactive";
    });
    if (found && !unit.sourceFile.empty()) result.push_back(unit.sourceFile.generic_string());
    if (result.size() >= limit) break;
  }
  return result;
}

struct InferredType {
  std::string type;
  std::string source;
  std::string confidence{"low"};
};

InferredType inferType(const UnitAst& ast, std::string expression,
                       const std::vector<IndexedUnit>& index, std::size_t sourceOffset = 0) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= expression.size()) {
    const auto dot = expression.find('.', start);
    parts.push_back(expression.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  if (parts.empty()) return {};
  const auto key = lower(parts.front());
  std::vector<InferredType> matches;
  const AstDeclaration* enclosingRoutine = nullptr;
  for (const auto& declaration : ast.declarations) {
    const auto kind = lower(declaration.kind);
    if (kind != "procedure" && kind != "function" && kind != "constructor" && kind != "destructor") continue;
    if (declaration.range.begin.offset <= sourceOffset &&
        (!enclosingRoutine || declaration.range.begin.offset >= enclosingRoutine->range.begin.offset))
      enclosingRoutine = &declaration;
  }
  if (enclosingRoutine) for (const auto& parameter : enclosingRoutine->parameters)
    if (lower(parameter.name) == key && !parameter.type.empty())
      matches.push_back({parameter.type, "parameter", "high"});
  for (const auto& declaration : ast.declarations) {
    if (lower(simpleName(declaration.name)) == key && !declaration.type.empty() &&
        (lower(declaration.kind) != "localvariable" ||
         (enclosingRoutine && lower(declaration.scope) == lower(enclosingRoutine->name)))) {
      const auto kind = lower(declaration.kind);
      matches.push_back({declaration.type,
          kind == "property" ? "property" : kind == "field" ? "field" :
          declaration.visibility == "private" ? "local_variable" : "global", "high"});
    }
  }
  if (matches.empty()) for (const auto& unit : index) for (const auto& declaration : unit.declarations) {
    if (lower(simpleName(declaration.name)) != key || declaration.type.empty()) continue;
    const auto kind = lower(declaration.kind);
    if (kind == "variable" || kind == "field" || kind == "property")
      matches.push_back({declaration.type, kind == "property" ? "property" : kind == "field" ? "field" : "global", "high"});
  }
  if (matches.empty()) return {};
  const auto type = lower(matches.front().type);
  if (std::any_of(matches.begin() + 1, matches.end(), [&](const InferredType& item) {
        return lower(item.type) != type;
      })) return {};
  auto result = matches.front();
  for (std::size_t part = 1; part < parts.size(); ++part) {
    std::vector<std::string> memberTypes;
    std::set<std::string> acceptedOwners{lower(result.type)};
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto& unit : index) for (const auto& relation : unit.inheritance)
        if (acceptedOwners.contains(lower(relation.type)) && !relation.baseType.empty() &&
            acceptedOwners.insert(lower(relation.baseType)).second) changed = true;
    }
    for (const auto& unit : index) for (const auto& declaration : unit.declarations)
      if (acceptedOwners.contains(lower(declaration.ownerType)) &&
          lower(simpleName(declaration.name)) == lower(parts[part]) && !declaration.type.empty())
        memberTypes.push_back(declaration.type);
    if (memberTypes.empty()) return {};
    const auto memberType = lower(memberTypes.front());
    if (std::any_of(memberTypes.begin() + 1, memberTypes.end(), [&](const std::string& candidate) {
          return lower(candidate) != memberType;
        })) return {};
    result = {memberTypes.front(), "property", "high"};
  }
  return result;
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

void writePerformance(std::ostringstream& output, QueryPerformance performance,
                      std::chrono::steady_clock::time_point started) {
  performance.totalMs += std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started).count() + performance.indexLoadMs + performance.packageEnrichmentMs;
  output << "\"query_performance\":{\"index_load_ms\":" << performance.indexLoadMs
         << ",\"cache_status\":\"" << escape(performance.cacheStatus) << "\""
         << ",\"startup_ms\":" << performance.startupMs
         << ",\"index_open_ms\":" << performance.indexOpenMs
         << ",\"index_parse_ms\":" << performance.indexParseMs
         << ",\"cache_load_ms\":" << performance.cacheLoadMs
         << ",\"cache_build_ms\":" << performance.cacheBuildMs
         << ",\"symbol_lookup_ms\":" << performance.symbolLookupMs
         << ",\"index_lookup_ms\":" << performance.indexLookupMs
         << ",\"source_parse_ms\":" << performance.sourceParseMs
         << ",\"source_scan_ms\":" << performance.sourceScanMs
         << ",\"package_enrichment_ms\":" << performance.packageEnrichmentMs
         << ",\"legacy_export_lookup_ms\":" << performance.legacyExportLookupMs
         << ",\"mapping_lookup_ms\":" << performance.mappingLookupMs
         << ",\"v500_type_lookup_ms\":" << performance.v500TypeLookupMs
         << ",\"v600_type_lookup_ms\":" << performance.v600TypeLookupMs
         << ",\"target_member_analysis_ms\":" << performance.targetMemberAnalysisMs
         << ",\"json_serialize_ms\":0,\"total_ms\":" << performance.totalMs
         << ",\"cache_hit\":" << (performance.cacheHit ? "true" : "false") << '}';
}

std::string timeoutJson(QueryPerformance performance, std::chrono::steady_clock::time_point started) {
  std::ostringstream output;
  output << "{\"schema_version\":\"2.2\",\"diagnosis\":{\"classification\":\"unresolved\","
            "\"confidence\":\"low\",\"recommended_action\":\"review_required\",\"diagnostics\":[{"
            "\"code\":\"query_timeout\",\"severity\":\"warning\","
            "\"message\":\"Semantic query exceeded its time budget.\"}]},";
  writePerformance(output, performance, started);
  output << '}';
  return output.str();
}

}  // namespace

std::string compilerLogJson(const std::filesystem::path& input, QueryPerformance performance) {
  const auto started = std::chrono::steady_clock::now();
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
  const auto actionable = std::find_if(errors.begin(), errors.end(), [](const Error& error) {
    const auto severity = lower(error.severity);
    const auto code = lower(error.code);
    return (severity == "error" || severity == "fatal") &&
           !code.empty() && (code.front() == 'e' || code.front() == 'f');
  });
  if (actionable == errors.end()) output << "null";
  else output << "{\"ordinal\":" << std::distance(errors.begin(), actionable) + 1
              << ",\"file\":\"" << escape(actionable->file)
              << "\",\"line\":" << actionable->line << ",\"code\":\"" << escape(actionable->code) << "\"}";
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
  output << "],"; writePerformance(output, performance, started); output << '}';
  return output.str();
}

std::string legacyReferencesJson(const std::filesystem::path& file, std::string_view legacyUnit,
                                 const std::vector<IndexedUnit>& left, const std::vector<IndexedUnit>& right,
                                 const std::filesystem::path& unitMap, QueryPerformance performance) {
  const auto started = std::chrono::steady_clock::now();
  const auto parseStarted = std::chrono::steady_clock::now();
  const auto source = readFile(file);
  const auto ast = parseUnit(file, source);
  performance.sourceParseMs += std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - parseStarted).count();
  const auto lookupStarted = std::chrono::steady_clock::now();
  const auto legacy = findUnit(left, legacyUnit);
  performance.legacyExportLookupMs += std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - lookupStarted).count();
  const auto mappingStarted = std::chrono::steady_clock::now();
  const auto unitStatus = mappingStatus(unitMap, legacyUnit);
  performance.mappingLookupMs += std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - mappingStarted).count();
  const bool importsLegacy = std::any_of(ast.uses.begin(), ast.uses.end(), [&](const UsesItem& item) {
    return lower(item.name) == lower(std::string(legacyUnit));
  });
  std::unordered_set<std::string> exports;
  if (legacy) for (const auto& symbol : legacy->symbols) exports.insert(lower(symbol));
  struct Ref {
    std::string symbol, kind, usage, status, ownerUnit, ownerSource, targetUnit, action;
    SourceRange range;
    std::vector<std::string> candidates;
  };
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
    std::vector<const IndexedUnit*> v500Owners;
    for (const auto& unit : left) if (unitDeclaresSymbol(unit, simple)) v500Owners.push_back(&unit);
    const IndexedUnit* provenOwner = v500Owners.size() == 1 ? v500Owners.front() : nullptr;
    if (!exactLegacyOwner && !qualifiedLegacy && !provenOwner && call == ast.calls.end()) continue;
    if (genericLegacySymbol(simple) && (!exactLegacyOwner || v500Owners.size() != 1)) {
      ++ignoredGenericReferences;
      continue;
    }
    Ref item{simple, "unknown", usageFor(ast, reference), provenOwner ? "legacy_symbol_unmapped" : "ambiguous",
             provenOwner ? provenOwner->name : qualifiedLegacy ? std::string(legacyUnit) : std::string{},
             provenOwner ? provenOwner->sourceFile.generic_string() : std::string{}, {}, "manual_required",
             reference.range, {}};
    const auto* declarationOwner = provenOwner ? provenOwner : legacy;
    if (declarationOwner) for (const auto& declaration : declarationOwner->declarations)
      if (lower(simpleName(declaration.name)) == lower(simple)) { item.kind = declaration.kind; break; }
    if (provenOwner) for (const auto& unit : right)
      if (unitDeclaresSymbol(unit, simple)) item.candidates.push_back(unit.name);
    const auto sourceEvidence = provenOwner && item.candidates.empty() ? activeSourceEvidence(right, simple) : std::vector<std::string>{};
    if (provenOwner && item.candidates.size() == 1) {
      item.status = "verified";
      item.targetUnit = item.candidates.front();
      item.action = "add_uses";
    }
    else if (!item.candidates.empty() || !sourceEvidence.empty()) item.status = "ambiguous";
    if (provenOwner || qualifiedLegacy) references.push_back(std::move(item));
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
      Ref item{member, "unknown", "invocation", "ambiguous", {}, {}, {}, "manual_required", call.range, {}};
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
           << ",\"usage\":\"" << reference.usage << "\",\"v500_owner\":";
    writeNullable(output, reference.ownerUnit);
    output << ",\"v500_owner_unit\":"; writeNullable(output, reference.ownerUnit);
    output << ",\"v500_owner_source\":"; writeNullable(output, reference.ownerSource);
    output << ",\"v600_owner_unit\":"; writeNullable(output, reference.targetUnit);
    output << ",\"v600_symbol\":"; writeNullable(output, reference.targetUnit.empty() ? "" : reference.symbol);
    output << ",\"v600_candidates\":[";
    for (std::size_t c = 0; c < reference.candidates.size(); ++c) {
      if (c) output << ',';
      output << '"' << escape(reference.candidates[c]) << '"';
    }
    output << "],\"mapping_status\":\"" << reference.status
           << "\",\"automatic_action\":\"" << reference.action << "\"}";
  }
  output << "],\"unresolved_active_references\":[";
  for (std::size_t i = 0; i < unresolvedReferences.size(); ++i) {
    if (i) output << ',';
    const auto& reference = unresolvedReferences[i];
    output << "{\"symbol\":\"" << escape(reference.symbol) << "\",\"line\":" << reference.range.begin.line
           << ",\"column\":" << reference.range.begin.column << ",\"usage\":\"" << reference.usage
           << "\",\"possible_v500_owner\":";
    writeNullable(output, reference.ownerUnit);
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
  const Ref* blocker = nullptr;
  for (const auto& reference : references)
    if (reference.status == "legacy_symbol_unmapped") { blocker = &reference; break; }
  if (!blocker && !unresolvedReferences.empty()) blocker = &unresolvedReferences.front();
  if (!blocker && importsLegacy) output << "\"legacy_unit_exports_incomplete\"";
  else if (!blocker) output << "null";
  else output << '"' << escape(blocker->status + ":" + blocker->symbol) << '"';
  output << "},"; writePerformance(output, performance, started); output << '}';
  return output.str();
}

std::string modelMigrationJson(const std::filesystem::path& file, std::string_view symbol,
                               const std::vector<IndexedUnit>& left, const std::vector<IndexedUnit>& right,
                               const std::filesystem::path&, QueryPerformance performance) {
  const auto started = std::chrono::steady_clock::now();
  const auto parseStarted = std::chrono::steady_clock::now();
  const auto source = readFile(file);
  const auto ast = parseUnit(file, source);
  performance.sourceParseMs += std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - parseStarted).count();
  const IndexedUnit* oldOwner = nullptr;
  const IndexedUnit* newOwner = nullptr;
  const auto leftLookupStarted = std::chrono::steady_clock::now();
  for (const auto& used : ast.uses) {
    const auto* unit = findUnit(left, used.name);
    if (unit && (unitDeclaresSymbol(*unit, symbol) || std::any_of(unit->inheritance.begin(), unit->inheritance.end(), [&](const auto& relation) {
          return lower(relation.type) == lower(std::string(symbol));
        }))) {
      oldOwner = unit;
      break;
    }
  }
  for (const auto& unit : left)
    if (!oldOwner && unitDeclaresSymbol(unit, symbol)) { oldOwner = &unit; break; }
  performance.v500TypeLookupMs += std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - leftLookupStarted).count();
  const auto rightLookupStarted = std::chrono::steady_clock::now();
  for (const auto& unit : right)
    if (unitDeclaresSymbol(unit, symbol)) { newOwner = &unit; break; }
  const auto probableUnit = lower(std::string(symbol).starts_with("T") ? std::string(symbol).substr(1) : std::string(symbol));
  for (const auto& unit : left)
    if (!oldOwner && lower(simpleName(unit.name)) == probableUnit && unitDeclaresSymbol(unit, symbol)) { oldOwner = &unit; break; }
  for (const auto& unit : right)
    if (!newOwner && lower(simpleName(unit.name)) == probableUnit && unitDeclaresSymbol(unit, symbol)) { newOwner = &unit; break; }
  performance.v600TypeLookupMs += std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - rightLookupStarted).count();
  const auto memberStarted = std::chrono::steady_clock::now();
  static const std::vector<std::string> modelMembers = {
      "EnableOnChange", "FieldValues", "GetCsField", "CSSeek", "CSModify", "CSResetCampi"};
  std::vector<std::string> used;
  for (const auto& member : modelMembers) {
    const std::regex token("\\b" + member + "\\b", std::regex::icase);
    if (std::regex_search(source, token)) used.push_back(member);
  }
  std::vector<std::string> compatible, missing;
  std::vector<std::string> exported;
  if (oldOwner) for (const auto& member : modelMembers)
    if (unitDeclaresSymbol(*oldOwner, member)) exported.push_back(member);
  for (const auto& member : used) {
    const bool exists = newOwner && (std::any_of(newOwner->symbols.begin(), newOwner->symbols.end(), [&](const std::string& name) {
      return lower(name) == lower(member);
    }) || std::any_of(newOwner->declarations.begin(), newOwner->declarations.end(), [&](const AstDeclaration& declaration) {
      return lower(simpleName(declaration.name)) == lower(member);
    }));
    (exists ? compatible : missing).push_back(member);
  }
  performance.targetMemberAnalysisMs += std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - memberStarted).count();
  const auto writeArray = [](std::ostringstream& out, const std::vector<std::string>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) { if (i) out << ','; out << '"' << escape(values[i]) << '"'; }
  };
  std::ostringstream output;
  output << "{\"schema_version\":\"2.2\",\"model\":\"" << escape(symbol) << "\",\"v500\":{\"unit\":";
  writeNullable(output, oldOwner ? oldOwner->name : "");
  output << ",\"source_file\":";
  writeNullable(output, oldOwner ? oldOwner->sourceFile.generic_string() : "");
  output << ",\"type_exists\":" << (oldOwner ? "true" : "false") << ",\"members_used\":["; writeArray(output, used);
  output << "],\"exported_members\":["; writeArray(output, exported);
  output << "]},\"v600\":{\"type_exists\":" << (newOwner ? "true" : "false") << ",\"unit\":";
  writeNullable(output, newOwner ? newOwner->name : "");
  output << ",\"source_file\":";
  writeNullable(output, newOwner ? newOwner->sourceFile.generic_string() : "");
  output << ",\"compatible_members\":["; writeArray(output, compatible);
  output << "],\"missing_or_unresolved_members\":["; writeArray(output, missing);
  output << "]},\"classification\":\"legacy_model_migration_required\",\"automatic_action\":\"manual_required\",";
  writePerformance(output, performance, started); output << '}';
  return output.str();
}

std::string diagnoseJson(const DiagnoseOptions& options, const std::vector<IndexedUnit>& right,
                         const std::vector<IndexedUnit>& left) {
  const auto started = std::chrono::steady_clock::now();
  auto performance = options.performance;
  const auto parseStarted = std::chrono::steady_clock::now();
  const auto source = readFile(options.file);
  const auto ast = parseUnit(options.file, source);
  performance.sourceParseMs += std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - parseStarted).count();
  if (performance.indexLoadMs + performance.packageEnrichmentMs + performance.sourceParseMs >=
      static_cast<long long>(options.timeoutMs)) return timeoutJson(performance, started);
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
        auto actual = inferType(ast, call.arguments[i].text, right, call.range.begin.offset).type;
        if (actual.empty()) actual = call.arguments[i].resolvedType;
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
  struct Candidate {
    const IndexedUnit* unit;
    const AstDeclaration* declaration;
    int score;
    std::string compatibility;
    std::string qualifierMatch;
  };
  std::vector<Candidate> candidates;
  std::set<std::string> uses;
  for (const auto& used : ast.uses) uses.insert(lower(used.name));
  const std::string receiverExpression = selected && selected->name.find('.') != std::string::npos
      ? selected->name.substr(0, selected->name.rfind('.')) : std::string{};
  const auto sourceMember = selected ? simpleName(selected->name) : requested;
  auto receiverInfo = inferType(ast, receiverExpression, right, selected ? selected->range.begin.offset : 0);
  auto receiverType = receiverInfo.type;
  if (receiverType.empty() && !receiverExpression.empty()) {
    const auto receiverName = simpleName(receiverExpression);
    const auto classReceiver = std::any_of(right.begin(), right.end(), [&](const IndexedUnit& unit) {
      return std::any_of(unit.declarations.begin(), unit.declarations.end(), [&](const AstDeclaration& declaration) {
        return lower(declaration.ownerType) == lower(receiverName);
      });
    });
    if (classReceiver) receiverType = receiverName;
  }
  const IndexedUnit* sourceOwner = nullptr;
  if (!receiverExpression.empty()) for (const auto& unit : left) {
    if (!unitDeclaresSymbol(unit, simpleName(receiverExpression)) &&
        !std::any_of(unit.inheritance.begin(), unit.inheritance.end(), [&](const auto& relation) {
          return lower(relation.type) == lower(simpleName(receiverExpression));
        })) continue;
    if (sourceOwner) { sourceOwner = nullptr; break; }
    sourceOwner = &unit;
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
    const bool mappedOwner = sourceOwner && std::any_of(mappings.begin(), mappings.end(), [&](const UnitMappingSuggestion& mapping) {
      return lower(mapping.sourceUnit) == lower(sourceOwner->name) && lower(mapping.targetUnit) == lower(unit.name);
    });
    if (uses.contains(lower(unit.name))) score += 20;
    if (mappedOwner) score += 30;
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
        auto actual = inferType(ast, selected->arguments[i].text, right, selected->range.begin.offset).type;
        if (actual.empty()) actual = selected->arguments[i].resolvedType;
        if (actual.empty() || declaration.parameters[i].type.empty()) known = false;
        else if (!typeCompatible(actual, declaration.parameters[i].type)) compatible = false;
      }
      compatibility = !known ? "unknown" : compatible ? "compatible" : "incompatible";
      if (compatible && known) score += 10;
    }
    if (!declaration.signature.empty()) score += 5;
    candidates.push_back({&unit, &declaration, std::min(score, 100), compatibility,
                          ownerMatch || unitMatch ? "exact" : mappedOwner ? "mapped" : "none"});
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
  const Candidate* selectedCandidate = candidates.empty() ||
      (candidates.size() > 1 && candidates[0].score == candidates[1].score) ? nullptr : &candidates.front();
  if (selectedCandidate && selected && !selected->arguments.empty()) {
    auto actualFirst = inferType(ast, selected->arguments.front().text, right, selected->range.begin.offset).type;
    if (actualFirst.empty()) actualFirst = selected->arguments.front().resolvedType;
    bool acceptsCurrent = false, acceptsConnection = false;
    for (const auto& candidate : candidates) {
      if (candidate.unit != selectedCandidate->unit ||
          candidate.declaration->parameters.size() != selected->arguments.size()) continue;
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
    bool legacyFound = false;
    std::size_t currentOwners = 0;
    for (const auto& unit : left) legacyFound = legacyFound || unitDeclaresSymbol(unit, requested);
    for (const auto& unit : right) if (unitDeclaresSymbol(unit, requested)) ++currentOwners;
    const bool currentFound = currentOwners > 0;
    const auto currentSourceEvidence = currentFound ? std::vector<std::string>{} : activeSourceEvidence(right, requested);
    if (currentOwners > 1) {
      v600OwnerAmbiguous = true;
      classification = "ambiguous"; action = "review_required"; confidence = "medium";
    } else if (currentOwners == 1 && !legacyFound) {
      classification = "unit_mapping"; action = "add_uses"; confidence = "high";
    }
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
  std::size_t safeArgumentIndex = 0;
  if (selectedCandidate && selected) {
    for (std::size_t i = 0; i < selected->arguments.size() && i < selectedCandidate->declaration->parameters.size(); ++i) {
      auto actual = inferType(ast, selected->arguments[i].text, right, selected->range.begin.offset).type;
      if (actual.empty()) actual = selected->arguments[i].resolvedType;
      const auto expected = selectedCandidate->declaration->parameters[i].type;
      if (lower(actual) != "taziendastd" ||
          (lower(expected) != "tcsedatabase" && lower(expected) != "ierpconnection")) continue;
      const bool acceptsCurrent = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
        return candidate.declaration->parameters.size() == selected->arguments.size() &&
               i < candidate.declaration->parameters.size() &&
               typeCompatible(actual, candidate.declaration->parameters[i].type);
      });
      const auto connection = inferType(ast, selected->arguments[i].text + ".Connection", right,
                                        selected->range.begin.offset);
      if (!acceptsCurrent && typeCompatible(connection.type, expected)) {
        safeConnectionReplacement = true;
        safeArgumentIndex = i;
      }
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
  writeNullable(output, receiverExpression); output << ",\"type\":"; writeNullable(output, receiverType);
  output << ",\"type_confidence\":\"" << (receiverType.empty() ? "low" : "high")
         << "\",\"type_source\":";
  writeNullable(output, receiverType.empty() ? "" : receiverInfo.source.empty() ? "inferred" : receiverInfo.source);
  output << "},\"source_owner\":{\"name\":";
  writeNullable(output, sourceOwner ? simpleName(receiverExpression) : "");
  output << ",\"kind\":"; writeNullable(output, sourceOwner ? "class" : "");
  output << ",\"v500_unit\":"; writeNullable(output, sourceOwner ? sourceOwner->name : "");
  output << ",\"v500_source_file\":"; writeNullable(output, sourceOwner ? sourceOwner->sourceFile.generic_string() : "");
  output << ",\"v500_package\":{\"name\":"; writeNullable(output, sourceOwner ? sourceOwner->packageName : "");
  output << ",\"dcp\":"; writeNullable(output, sourceOwner ? sourceOwner->packageDcp : "");
  output << ",\"bpl\":"; writeNullable(output, sourceOwner ? sourceOwner->packageBpl : "");
  output << "}},\"mapped_owner_candidates\":[";
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (i) output << ',';
    const auto& candidate = candidates[i];
    const bool mappedOwner = sourceOwner && std::any_of(mappings.begin(), mappings.end(), [&](const UnitMappingSuggestion& mapping) {
      return lower(mapping.sourceUnit) == lower(sourceOwner->name) && lower(mapping.targetUnit) == lower(candidate.unit->name);
    });
    output << "{\"v600_owner\":\"" << escape(candidate.unit->name) << "\",\"v600_unit\":\""
           << escape(candidate.unit->name) << "\",\"mapping_source\":\""
           << (mappedOwner ? "unit_map" : !candidate.unit->packageName.empty() ? "package_relation" : "export_match")
           << "\",\"confidence\":\"" << (selectedCandidate == &candidate ? "high" : "low") << "\"}";
  }
  output << "],\"callee_candidates\":[";
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
           << candidate.compatibility << "\",\"qualifier_match\":\"" << candidate.qualifierMatch << "\"}";
  }
  output << "],\"arguments\":[";
  if (selected) for (std::size_t i = 0; i < selected->arguments.size(); ++i) {
    if (i) output << ',';
    auto inferred = inferType(ast, selected->arguments[i].text, right, selected->range.begin.offset);
    auto actual = inferred.type.empty() ? selected->arguments[i].resolvedType : inferred.type;
    if (!selected->arguments[i].resolvedType.empty() && inferred.type.empty())
      inferred = {selected->arguments[i].resolvedType, "inferred", "high"};
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
    output << ",\"type_confidence\":\"" << (actual.empty() ? "low" : inferred.confidence)
           << "\",\"type_source\":";
    writeNullable(output, actual.empty() ? "" : inferred.source.empty() ? "inferred" : inferred.source);
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
           << "\",\"assignment_target\":"; writeNullable(output, selected ? selected->assignmentTarget.text : "");
    output << ",\"assignment_target_type\":"; writeNullable(output, selected ? selected->assignmentTarget.resolvedType : "");
    output << ",\"unit\":"; writeNullable(output, selectedCandidate ? selectedCandidate->unit->name : "");
    output << ",\"package\":{\"name\":"; writeNullable(output, selectedCandidate ? selectedCandidate->unit->packageName : "");
    output << ",\"dcp\":"; writeNullable(output, selectedCandidate ? selectedCandidate->unit->packageDcp : "");
    output << ",\"bpl\":"; writeNullable(output, selectedCandidate ? selectedCandidate->unit->packageBpl : "");
    output << "},\"automatic_migration_safe\":false,\"recommended_action\":\"review_required\","
              "\"reason\":\"Return type changes error-handling semantics.\"}";
  }
  output << ",\"suggested_fixes\":[";
  bool wroteFix = false;
  if (safeConnectionReplacement && selected && safeArgumentIndex < selected->arguments.size()) {
    const auto& argument = selected->arguments[safeArgumentIndex];
    output << "{\"kind\":\"replace_argument\",\"confidence\":\"high\",\"file\":\""
           << escape(options.file.generic_string()) << "\",\"range\":{\"start_offset\":" << argument.range.begin.offset
           << ",\"end_offset\":" << argument.range.end.offset << ",\"start_line\":" << argument.range.begin.line
           << ",\"start_column\":" << argument.range.begin.column << ",\"end_line\":" << argument.range.end.line
           << ",\"end_column\":" << argument.range.end.column << "},\"replacement\":\""
           << escape(argument.text + ".Connection") << "\",\"preconditions\":[\"unique_callee\","
              "\"exact_member_match\",\"matching_argument_count\",\"source_type_proven\","
              "\"target_type_proven\",\"connection_property_resolved\",\"no_compatible_source_overload\"],"
              "\"evidence\":{\"source_type\":\"TAziendaStd\",\"target_type\":\""
           << escape(selectedCandidate->declaration->parameters[safeArgumentIndex].type) << "\"}}";
    wroteFix = true;
  }
  if (!wroteFix && options.errorCode == "F2613" && mapped != mappings.end() &&
      !mapped->targetUnit.empty() && mapped->confidence == "high" &&
      mapped->compatibility == "full_compatible" && mapped->automaticAction == "replace_in_uses") {
    const auto used = std::find_if(ast.uses.begin(), ast.uses.end(), [&](const UsesItem& item) {
      return lower(item.name) == lower(mapped->sourceUnit);
    });
    if (used != ast.uses.end()) {
      output << "{\"kind\":\"replace_in_uses\",\"confidence\":\"high\",\"file\":\""
             << escape(options.file.generic_string()) << "\",\"range\":{\"start_offset\":" << used->range.begin.offset
             << ",\"end_offset\":" << used->range.end.offset << ",\"start_line\":" << used->range.begin.line
             << ",\"start_column\":" << used->range.begin.column << ",\"end_line\":" << used->range.end.line
             << ",\"end_column\":" << used->range.end.column << "},\"replacement\":\""
             << escape(mapped->targetUnit) << "\",\"preconditions\":[\"approved_unit_mapping\","
                "\"full_compatible\",\"exact_uses_range\"]}";
    }
  }
  output << ']';
  output << ','; writePerformance(output, performance, started); output << '}';
  return output.str();
}

std::string typeInfoJson(std::string_view requested, const std::vector<IndexedUnit>& index,
                         bool includeInherited, bool includeOverloads, QueryPerformance performance) {
  const auto started = std::chrono::steady_clock::now();
  std::vector<const IndexedUnit*> owners;
  std::vector<const IndexedUnit*> exactOwners;
  for (const auto& unit : index) {
    const bool declaredType = std::any_of(unit.declarations.begin(), unit.declarations.end(), [&](const AstDeclaration& declaration) {
      const auto kind = lower(declaration.kind);
      return lower(simpleName(declaration.name)) == lower(std::string(requested)) &&
             (kind == "class" || kind == "interface");
    });
    const bool indexedTypeCandidate = std::any_of(unit.declarations.begin(), unit.declarations.end(), [&](const AstDeclaration& declaration) {
      return lower(simpleName(declaration.name)) == lower(std::string(requested)) && lower(declaration.kind) == "type";
    }) || std::any_of(unit.inheritance.begin(), unit.inheritance.end(), [&](const InheritanceRelation& relation) {
      return lower(relation.type) == lower(std::string(requested));
    });
    if (declaredType) exactOwners.push_back(&unit);
    if (declaredType || indexedTypeCandidate) owners.push_back(&unit);
  }
  if (!exactOwners.empty()) owners = std::move(exactOwners);
  const IndexedUnit* owner = owners.size() == 1 ? owners.front() : nullptr;
  std::vector<std::string> ancestors;
  if (owner) for (const auto& relation : owner->inheritance)
    if (lower(relation.type) == lower(std::string(requested))) ancestors.push_back(relation.baseType);

  struct Member { const IndexedUnit* unit; AstDeclaration declaration; bool inherited; };
  std::vector<Member> methods, properties;
  std::set<std::string> acceptedOwners{lower(std::string(requested))};
  if (includeInherited) {
    for (const auto& ancestor : ancestors) acceptedOwners.insert(lower(ancestor));
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto& unit : index) for (const auto& relation : unit.inheritance)
        if (acceptedOwners.contains(lower(relation.type)) && !relation.baseType.empty() &&
            acceptedOwners.insert(lower(relation.baseType)).second) changed = true;
    }
  }
  auto collect = [&](const IndexedUnit& unit, const AstDeclaration& declaration) {
    if (declaration.ownerType.empty() || !acceptedOwners.contains(lower(declaration.ownerType))) return;
    const bool inherited = lower(declaration.ownerType) != lower(std::string(requested));
    if (inherited && !includeInherited) return;
    if (!inherited && &unit != owner) return;
    const auto duplicate = [&](const Member& member) {
      return lower(declarationSignature(member.declaration)) == lower(declarationSignature(declaration)) &&
             lower(member.unit->name) == lower(unit.name);
    };
    if (std::any_of(methods.begin(), methods.end(), duplicate) ||
        std::any_of(properties.begin(), properties.end(), duplicate)) return;
    if (lower(declaration.kind) == "property" || lower(declaration.kind) == "field")
      properties.push_back({&unit, declaration, inherited});
    else if (includeOverloads || !std::any_of(methods.begin(), methods.end(), [&](const Member& member) {
      return lower(simpleName(member.declaration.name)) == lower(simpleName(declaration.name));
    })) methods.push_back({&unit, declaration, inherited});
  };
  if (owner) {
    for (const auto& unit : index) for (const auto& declaration : unit.declarations) collect(unit, declaration);
  }
  std::ostringstream output;
  output << "{\"schema_version\":\"2.3\",\"type\":\"" << escape(requested) << "\",\"unit\":";
  writeNullable(output, owner ? owner->name : "");
  output << ",\"source_file\":"; writeNullable(output, owner ? owner->sourceFile.generic_string() : "");
  output << ",\"package\":{\"name\":"; writeNullable(output, owner ? owner->packageName : "");
  output << ",\"dcp\":"; writeNullable(output, owner ? owner->packageDcp : "");
  output << ",\"bpl\":"; writeNullable(output, owner ? owner->packageBpl : "");
  output << ",\"source_project\":"; writeNullable(output, owner ? owner->sourceProject.generic_string() : "");
  output << "},\"ancestors\":[";
  for (std::size_t i = 0; i < ancestors.size(); ++i) { if (i) output << ','; output << '"' << escape(ancestors[i]) << '"'; }
  output << "],\"methods\":[";
  for (std::size_t i = 0; i < methods.size(); ++i) {
    if (i) output << ',';
    const auto& member = methods[i];
    output << "{\"name\":\"" << escape(simpleName(member.declaration.name)) << "\",\"signature\":\""
           << escape(declarationSignature(member.declaration)) << "\",\"return_type\":";
    writeNullable(output, member.declaration.type);
    output << ",\"declared_in\":\"" << escape(member.unit->name) << "\",\"inherited\":"
           << (member.inherited ? "true" : "false") << ",\"overload\":"
           << (member.declaration.overload ? "true" : "false") << ",\"reintroduced\":"
           << (member.declaration.reintroduced ? "true" : "false") << ",\"deprecated\":"
           << (member.declaration.deprecated ? "true" : "false") << "}";
  }
  output << "],\"properties\":[";
  for (std::size_t i = 0; i < properties.size(); ++i) {
    if (i) output << ',';
    output << "{\"name\":\"" << escape(simpleName(properties[i].declaration.name)) << "\",\"type\":";
    writeNullable(output, properties[i].declaration.type);
    output << ",\"declared_in\":\"" << escape(properties[i].unit->name) << "\",\"inherited\":"
           << (properties[i].inherited ? "true" : "false") << "}";
  }
  output << "],\"classification\":\"" << (owner ? "provider_resolved" : owners.empty() ? "unresolved" : "ambiguous")
         << "\",\"diagnostics\":[";
  if (owners.size() > 1) output << "{\"code\":\"ambiguous_symbol_owner\",\"severity\":\"warning\",\"message\":\"Multiple units declare the requested type.\"}";
  else if (!owner) output << "{\"code\":\"unresolved_symbol\",\"severity\":\"warning\",\"message\":\"Type was not found in the persistent index or its recorded sources.\"}";
  output << "],"; writePerformance(output, performance, started); output << '}';
  return output.str();
}

std::string symbolOriginJson(std::string_view name, const std::vector<IndexedUnit>& index,
                             QueryPerformance performance) {
  const auto started = std::chrono::steady_clock::now();
  const bool hasV600 = std::any_of(index.begin(), index.end(), [](const IndexedUnit& unit) {
    return lower(unit.indexVersion) == "v600";
  });
  std::vector<const IndexedUnit*> providers;
  for (const auto& unit : index) if ((!hasV600 || lower(unit.indexVersion) != "v500") &&
      (unitDeclaresSymbol(unit, name) ||
      std::any_of(unit.inheritance.begin(), unit.inheritance.end(), [&](const auto& relation) {
        return lower(relation.type) == lower(std::string(name));
      }))) providers.push_back(&unit);
  struct Evidence { std::string file, usage, unit; std::size_t line{}, column{}; };
  std::vector<Evidence> evidence;
  for (const auto& unit : index) {
    if (hasV600 && lower(unit.indexVersion) == "v500") continue;
    for (const auto& item : unit.sourceEvidence) {
    if (lower(simpleName(item.symbol)) != lower(std::string(name))) continue;
    evidence.push_back({unit.sourceFile.generic_string(), item.usage, unit.name, item.line, item.column});
    if (evidence.size() >= 32) break;
    }
  }
  const std::string classification = providers.size() == 1 ? "provider_resolved" :
                                     (!providers.empty() || !evidence.empty()) ? "ambiguous" : "unresolved";
  std::ostringstream output;
  output << "{\"schema_version\":\"2.3\",\"symbol\":\"" << escape(name) << "\",\"active_source_evidence\":[";
  for (std::size_t i = 0; i < evidence.size(); ++i) {
    if (i) output << ',';
    output << "{\"file\":\"" << escape(evidence[i].file) << "\",\"line\":" << evidence[i].line
           << ",\"column\":" << evidence[i].column << ",\"usage\":\"" << escape(evidence[i].usage)
           << "\",\"active\":true,\"unit\":\"" << escape(evidence[i].unit) << "\"}";
  }
  output << "],\"provider_candidates\":[";
  for (std::size_t i = 0; i < providers.size(); ++i) {
    if (i) output << ',';
    output << "{\"unit\":\"" << escape(providers[i]->name) << "\",\"source_file\":";
    writeNullable(output, providers[i]->sourceFile.generic_string());
    output << ",\"exported\":true,\"confidence\":\"" << (providers.size() == 1 ? "high" : "low") << "\"}";
  }
  output << "],\"classification\":\"" << classification
         << "\",\"recommended_action\":\"review_required\",\"reason\":";
  if (classification == "ambiguous") output << "\"symbol_exists_in_v600_but_owner_unit_is_ambiguous\"";
  else output << "null";
  output << ','; writePerformance(output, performance, started); output << '}';
  return output.str();
}

std::string batchDiagnoseJson(const std::filesystem::path& input,
                              const std::vector<IndexedUnit>& right,
                              const std::vector<IndexedUnit>& left,
                              const std::filesystem::path& unitMap,
                              std::size_t timeoutMs,
                              QueryPerformance performance) {
  const auto started = std::chrono::steady_clock::now();
  const auto source = readFile(input);
  const auto stringField = [](std::string_view object, std::string_view key) {
    const std::regex pattern("\\\"" + std::string(key) + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::cmatch match;
    const std::string text(object);
    return std::regex_search(text.c_str(), match, pattern) ? match[1].str() : std::string{};
  };
  const auto numberField = [](std::string_view object, std::string_view key) {
    const std::regex pattern("\\\"" + std::string(key) + "\\\"\\s*:\\s*(\\d+)");
    std::cmatch match;
    const std::string text(object);
    return std::regex_search(text.c_str(), match, pattern) ? static_cast<std::size_t>(std::stoull(match[1].str())) : 0;
  };
  const auto requests = source.find("\"requests\"");
  const auto open = source.find('[', requests);
  if (requests == std::string::npos || open == std::string::npos)
    throw std::runtime_error("Invalid batch diagnose input");
  std::vector<std::string> results;
  std::unordered_map<std::string, std::vector<IndexedUnit>> projectEnvironments;
  std::unordered_map<std::string, std::string> requestCache;
  std::size_t position = open + 1;
  while (position < source.size()) {
    const auto objectOpen = source.find('{', position);
    if (objectOpen == std::string::npos) break;
    const auto objectClose = source.find('}', objectOpen);
    if (objectClose == std::string::npos) break;
    const auto object = std::string_view(source).substr(objectOpen, objectClose - objectOpen + 1);
    DiagnoseOptions options;
    options.file = stringField(object, "file");
    options.dproj = stringField(object, "dproj");
    if (options.file.is_relative()) options.file = input.parent_path() / options.file;
    if (!options.dproj.empty() && options.dproj.is_relative()) options.dproj = input.parent_path() / options.dproj;
    options.errorCode = stringField(object, "error_code");
    options.symbol = stringField(object, "symbol");
    options.line = numberField(object, "line");
    options.unitMap = unitMap;
    options.timeoutMs = timeoutMs;
    options.performance.cacheHit = true;
    try {
      if (options.file.empty() || options.line == 0 || options.errorCode.empty())
        throw std::runtime_error("Batch request requires file, line, and error_code");
      const std::vector<IndexedUnit>* environment = &right;
      if (!options.dproj.empty()) {
        const auto key = lower(std::filesystem::absolute(options.dproj).lexically_normal().generic_string());
        auto found = projectEnvironments.find(key);
        if (found == projectEnvironments.end()) {
          auto merged = right;
          ProjectOptions projectOptions;
          projectOptions.dprojFile = options.dproj;
          auto package = options.dproj;
          package.replace_extension(".dpk");
          const auto project = loadPackage(package, projectOptions);
          mergeProjectSymbols(merged, project.analyzer.analyze());
          found = projectEnvironments.emplace(key, std::move(merged)).first;
        }
        environment = &found->second;
      }
      const auto requestKey = lower(options.file.generic_string()) + "|" + std::to_string(options.line) + "|" +
          lower(options.errorCode) + "|" + lower(options.symbol) + "|" + lower(options.dproj.generic_string());
      const auto cached = requestCache.find(requestKey);
      if (cached != requestCache.end()) results.push_back(cached->second);
      else {
        auto result = diagnoseJson(options, *environment, left);
        requestCache.emplace(requestKey, result);
        results.push_back(std::move(result));
      }
    } catch (const std::exception& error) {
      results.push_back("{\"schema_version\":\"2.2\",\"diagnosis\":{\"classification\":\"unresolved\","
                        "\"confidence\":\"low\",\"recommended_action\":\"review_required\","
                        "\"diagnostics\":[{\"code\":\"query_execution_failed\",\"severity\":\"warning\","
                        "\"message\":\"" + escape(error.what()) + "\"}]},\"suggested_fixes\":[]}");
    }
    position = objectClose + 1;
    const auto arrayClose = source.find(']', position);
    const auto nextObject = source.find('{', position);
    if (arrayClose != std::string::npos && (nextObject == std::string::npos || arrayClose < nextObject)) break;
  }
  std::ostringstream output;
  output << "{\"schema_version\":\"2.3\",\"results\":[";
  for (std::size_t i = 0; i < results.size(); ++i) { if (i) output << ','; output << results[i]; }
  output << "],\"request_count\":" << results.size() << ",\"index_load_count\":" << (right.empty() ? 0 : 1)
         << ",\"v500_index_load_ms\":" << performance.v500IndexLoadMs
         << ",\"v600_index_load_ms\":" << performance.v600IndexLoadMs
         << ",\"unit_map_load_ms\":" << performance.unitMapLoadMs
         << ",\"cache_hits\":" << performance.cacheHits << ',';
  writePerformance(output, performance, started);
  output << '}';
  return output.str();
}

}  // namespace jdelphiast
