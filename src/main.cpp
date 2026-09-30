#include "jdelphiast/analyzer.hpp"
#include "jdelphiast/project.hpp"
#include "jdelphiast/queries.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

const auto processStarted = std::chrono::steady_clock::now();

std::string timestamp() {
  auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  if (const auto* epoch = std::getenv("SOURCE_DATE_EPOCH")) {
    try { now = static_cast<std::time_t>(std::stoll(epoch)); } catch (...) {}
  }
  std::tm value{};
#ifdef _WIN32
  localtime_s(&value, &now);
#else
  localtime_r(&now, &value);
#endif
  char buffer[20]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &value);
  return buffer;
}

void writeAtomic(const std::filesystem::path& path, std::string_view content) {
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary);
    if (!output) throw std::runtime_error("Cannot write " + temporary);
    output << content;
    output.flush();
    if (!output) throw std::runtime_error("Failed writing " + temporary);
  }
  std::error_code error;
  std::filesystem::remove(path, error);
  std::filesystem::rename(temporary, path);
}

std::vector<jdelphiast::IndexedUnit> loadIndexes(const std::vector<std::filesystem::path>& files,
                                                 jdelphiast::QueryPerformance* performance = nullptr) {
  const auto started = std::chrono::steady_clock::now();
  std::vector<jdelphiast::IndexedUnit> result;
  std::unordered_map<std::string, std::size_t> positions;
  const auto normalized = [](std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
  };
  bool allCacheHits = !files.empty();
  for (const auto& file : files) {
    bool cacheHit = false;
    const auto loaded = jdelphiast::loadSymbolIndexShared(file, &cacheHit);
    allCacheHits = allCacheHits && cacheHit;
    if (result.empty() && files.size() == 1) {
      result = *loaded;
      for (std::size_t i = 0; i < result.size(); ++i) positions[normalized(result[i].name)] = i;
      continue;
    }
    for (const auto& unit : *loaded) {
      const auto key = normalized(unit.name);
      const auto found = positions.find(key);
      if (found == positions.end()) {
        positions.emplace(key, result.size());
        result.push_back(unit);
      } else result[found->second] = unit;
    }
  }
  for (const auto& unit : jdelphiast::bundledSymbolIndex()) {
    const auto key = normalized(unit.name);
    if (!positions.contains(key)) {
      positions.emplace(key, result.size());
      result.push_back(unit);
    }
  }
  if (performance) {
    performance->indexLoadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    performance->cacheHit = allCacheHits;
  }
  return result;
}

std::vector<jdelphiast::IndexedUnit> loadFilteredIndexes(
    const std::vector<std::filesystem::path>& files, const std::vector<std::string>& terms,
    jdelphiast::QueryPerformance* performance = nullptr) {
  const auto started = std::chrono::steady_clock::now();
  std::vector<jdelphiast::IndexedUnit> result;
  const auto normalized = [](std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
  };
  bool allHits = !files.empty();
  bool rebuilt = false;
  for (const auto& file : files) {
    jdelphiast::QueryPerformance item;
    if (performance) item.timeoutMs = performance->timeoutMs;
    auto loaded = jdelphiast::loadSymbolIndexFiltered(file, terms, &item);
    allHits = allHits && item.cacheHit;
    rebuilt = rebuilt || item.cacheStatus == "rebuilt";
    if (performance) {
      performance->indexOpenMs += item.indexOpenMs;
      performance->indexParseMs += item.indexParseMs;
      performance->cacheLoadMs += item.cacheLoadMs;
      performance->cacheBuildMs += item.cacheBuildMs;
    }
    result.insert(result.end(), loaded.begin(), loaded.end());
  }
  for (const auto& unit : jdelphiast::bundledSymbolIndex()) {
    const auto matches = std::any_of(terms.begin(), terms.end(), [&](const std::string& term) {
      return normalized(unit.name).find(normalized(term)) != std::string::npos ||
          std::any_of(unit.symbols.begin(), unit.symbols.end(), [&](const std::string& symbol) {
            return normalized(symbol) == normalized(term);
          });
    });
    if (matches && std::none_of(result.begin(), result.end(), [&](const auto& existing) {
          return normalized(existing.name) == normalized(unit.name);
        })) result.push_back(unit);
  }
  if (performance) {
    performance->indexLoadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    performance->cacheHit = allHits;
    performance->cacheStatus = rebuilt ? "rebuilt" : allHits ? "hit" : "miss";
  }
  return result;
}

std::vector<std::string> diagnosticTerms(const std::filesystem::path& file,
                                         std::string_view explicitSymbol = {}, std::size_t line = 0) {
  std::vector<std::string> result;
  const auto normalized = [](std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
  };
  if (!explicitSymbol.empty()) result.emplace_back(explicitSymbol);
  std::ifstream input(file, std::ios::binary);
  if (!input) return result;
  const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const auto ast = jdelphiast::parseUnit(file, source);
  std::size_t maximumDepth = 0;
  if (line) for (const auto& call : ast.calls) {
    if (call.range.begin.line > line || call.range.end.line < line) continue;
    std::size_t depth = 0;
    for (const auto& parent : ast.calls)
      if (&parent != &call && parent.range.begin.offset <= call.range.begin.offset &&
          parent.range.end.offset >= call.range.end.offset) ++depth;
    maximumDepth = std::max(maximumDepth, depth);
  }
  for (const auto& call : ast.calls) {
    if (line && (call.range.begin.line > line || call.range.end.line < line)) continue;
    if (line) {
      std::size_t depth = 0;
      for (const auto& parent : ast.calls)
        if (&parent != &call && parent.range.begin.offset <= call.range.begin.offset &&
            parent.range.end.offset >= call.range.end.offset) ++depth;
      if (depth != maximumDepth) continue;
    }
    const auto dot = call.name.find_last_of('.');
    const auto member = call.name.substr(dot == std::string::npos ? 0 : dot + 1);
    static const std::unordered_map<std::string, bool> generic = {
        {"add", true}, {"create", true}, {"open", true}, {"close", true}, {"execute", true}};
    if (!generic.contains(normalized(member))) result.push_back(member);
    const auto firstDot = call.name.find('.');
    if (firstDot != std::string::npos) {
      const auto receiver = call.name.substr(0, firstDot);
      std::string receiverType;
      for (const auto& declaration : ast.declarations) {
        if (normalized(declaration.name) == normalized(receiver) && !declaration.type.empty()) receiverType = declaration.type;
        for (const auto& parameter : declaration.parameters)
          if (normalized(parameter.name) == normalized(receiver) && !parameter.type.empty()) receiverType = parameter.type;
      }
      result.push_back(receiverType.empty() ? receiver : receiverType);
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<std::string> batchDiagnosticTerms(const std::filesystem::path& input) {
  std::ifstream stream(input, std::ios::binary);
  if (!stream) return {};
  const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  std::vector<std::string> result;
  const std::regex field(R"json("(file|symbol)"\s*:\s*"([^"]+)")json");
  for (std::sregex_iterator item(source.begin(), source.end(), field), end; item != end; ++item) {
    if ((*item)[1].str() == "symbol") result.push_back((*item)[2].str());
    else {
      const auto objectStart = source.rfind('{', static_cast<std::size_t>((*item).position()));
      const auto objectEnd = source.find('}', static_cast<std::size_t>((*item).position()));
      std::size_t line = 0;
      if (objectStart != std::string::npos && objectEnd != std::string::npos) {
        const auto object = source.substr(objectStart, objectEnd - objectStart + 1);
        const std::regex linePattern(R"("line"\s*:\s*(\d+))");
        std::smatch lineMatch;
        if (std::regex_search(object, lineMatch, linePattern)) line = std::stoull(lineMatch[1].str());
      }
      auto requestFile = std::filesystem::path((*item)[2].str());
      if (requestFile.is_relative() && !std::filesystem::exists(requestFile))
        requestFile = input.parent_path() / requestFile;
      const auto terms = diagnosticTerms(requestFile, {}, line);
      result.insert(result.end(), terms.begin(), terms.end());
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::string queryTimeoutJson(std::string_view schema, std::string_view command,
                             std::string_view query, const jdelphiast::QueryPerformance& performance) {
  return "{\"schema_version\":\"" + std::string(schema) +
      "\",\"command\":\"" + std::string(command) + "\",\"status\":\"timeout\",\"query\":\"" +
      std::string(query) + "\",\"query_timeout_ms\":" + std::to_string(performance.timeoutMs) +
      ",\"timed_out\":true,\"result\":null,\"classification\":\"unresolved\",\"recommended_action\":\"review_required\","
      "\"diagnostics\":[{\"code\":\"query_timeout\",\"severity\":\"warning\","
      "\"message\":\"Semantic query exceeded its time budget.\"}],\"query_performance\":{"
      "\"cache_status\":\"" + performance.cacheStatus + "\",\"startup_ms\":" + std::to_string(performance.startupMs) +
      ",\"index_open_ms\":" + std::to_string(performance.indexOpenMs) +
      ",\"index_parse_ms\":" + std::to_string(performance.indexParseMs) +
      ",\"cache_load_ms\":" + std::to_string(performance.cacheLoadMs) +
      ",\"cache_build_ms\":" + std::to_string(performance.cacheBuildMs) +
      ",\"symbol_lookup_ms\":" + std::to_string(performance.symbolLookupMs) +
      ",\"index_load_ms\":" + std::to_string(performance.indexLoadMs) +
      ",\"index_lookup_ms\":" + std::to_string(performance.indexLookupMs) +
      ",\"source_parse_ms\":" + std::to_string(performance.sourceParseMs) +
      ",\"source_scan_ms\":" + std::to_string(performance.sourceScanMs) +
      ",\"package_enrichment_ms\":" + std::to_string(performance.packageEnrichmentMs) +
      ",\"json_serialize_ms\":0,\"total_ms\":" + std::to_string(performance.indexLoadMs + performance.packageEnrichmentMs) +
      ",\"cache_hit\":" + (performance.cacheHit ? "true" : "false") + "}}";
}

std::filesystem::path packageFromDproj(const std::filesystem::path& dproj) {
  auto package = dproj;
  package.replace_extension(".dpk");
  return package;
}

void printHelp() {
  std::cout << R"(DelphiAstTool - Delphi/Object Pascal semantic dependency analyzer

USAGE
  DelphiAstTool.exe /?
  DelphiAstTool.exe analyze --dproj <project.dproj> [options] --format json
  DelphiAstTool.exe --project <package.dpk> --output <report.ast.json> [options]
  DelphiAstTool.exe [--json] <unit1.pas> [unit2.pas ...]
  DelphiAstTool.exe index --source <path> [--source <path> ...] --output <index.jdi> [--version v500|v600]
  DelphiAstTool.exe exports (--unit <name> | --file <unit.pas>) [--index <index.jdi>] --format json
  DelphiAstTool.exe symbol --name <symbol> [filters] [--index <index.jdi>] --format json
  DelphiAstTool.exe expression --file <unit.pas> --line <number> [project options] --format json
  DelphiAstTool.exe hierarchy (--file <unit.pas> | --dproj <project.dproj>) --class <name> [options] --format json
  DelphiAstTool.exe unit-info --unit <name> [--index <index.jdi>] --format json
  DelphiAstTool.exe compare-symbol --left-index <v500.jdi> --right-index <v600.jdi> --symbol <name> --format json
  DelphiAstTool.exe compare-units --left-index <v500.jdi> --right-index <v600.jdi> --output <catalog.json> [options]
  DelphiAstTool.exe diagnose --file <unit.pas> --line <number> --error-code <code> --index <v600.jdi> [options]
  DelphiAstTool.exe legacy-refs --file <unit.pas> --legacy-unit <name> --left-index <v500.jdi> --right-index <v600.jdi> [options]
  DelphiAstTool.exe model-migration --file <unit.pas> --symbol <name> --left-index <v500.jdi> --right-index <v600.jdi> [options]
  DelphiAstTool.exe compiler-log --input <dcc32-build-log.txt> --format json
  DelphiAstTool.exe type-info --type <name> --index <index.jdi> [--include-inherited] [--include-overloads] --format json
   DelphiAstTool.exe symbol-origin --name <symbol> --index <index.jdi> --format json
   DelphiAstTool.exe semantic-rule-match --rule <id> --file <unit.pas> --line <number> --format json
   DelphiAstTool.exe batch-diagnose --input <requests.json> --index <v600.jdi> [options] --output <results.json>
  DelphiAstTool.exe build-query-index --index <file.jdi> [--index <file.jdi> ...] --format json

PROJECT ANALYSIS OPTIONS
  --project <file.dpk>       Delphi package to analyze.
  --dproj <file.dproj>      Project settings. With explicit 'analyze', the sibling DPK is inferred.
  --config <name>           Active build configuration. Default: Release.
  --platform <name>         Active target platform. Default: Win32.
  --index <file.jdi>        Persistent symbol index. May be repeated.
  --search-path <dir>       Additional Delphi unit search path. May be repeated.
  --include-path <dir>      Additional include search path. May be repeated.
  --output <file>           Write the project report atomically to this file.
  --unit-map <file.json>    Attach a previously generated compare-units catalog reference.
  --format json             Write schema v2 JSON to stdout. Accepted by semantic queries.
  --json                    Legacy direct-source JSON output.

SYMBOL QUERY FILTERS
  --unit <name>             Restrict exports/symbol/unit-info to a unit.
  --file <file.pas>         Analyze a specific source file.
  --name <symbol>           Symbol simple name.
  --qualified-name <name>   Fully qualified symbol name.
  --kind <kind>             Restrict symbol kind.
  --line <number>           One-based line for expression analysis.
  --class <name>            Class to inspect in a hierarchy query.

INDEX OPTIONS
  --source <path>           Pascal file or source tree. May be repeated.
  --version <v500|v600>     Catalog version persisted in the JDI.
  --left-index <file.jdi>   Legacy side of compare-symbol.
  --right-index <file.jdi>  Destination side of compare-symbol.
  --origin <text>           Catalog source origin persisted by index.
  --source-root <path>      Additional source root scanned by index.
  --package-root <path>     Root searched for real DPK metadata.
  --validation-policy <file> Override diagnostic severity with code=warning|error lines.

COMPARE-UNITS OPTIONS
  --seed <file.json>        Approved mappings, evaluated before heuristics.
  --include-unmapped        Include not_found entries in mappings.
  --include-ambiguous       Include ambiguous entries in mappings.
  --include-symbol-details  Include export and signature evidence.
  --minimum-confidence <low|medium|high>
  --csv <file.csv>          Optional CSV report.
  --html <file.html>        Optional searchable HTML report.

DIAGNOSTIC OPTIONS
   --error-code <code>       Delphi compiler error associated with diagnose.
   --rule <id>               Semantic migration rule inspected read-only.
  --symbol <name>           Optional compiler symbol or model name.
  --legacy-unit <name>      Legacy owner inspected by legacy-refs.
  --input <build.log>       DCC32 log parsed by compiler-log.
  --type <name>             Type inspected by type-info.
  --include-inherited       Include members declared by indexed ancestors.
  --include-overloads       Preserve every indexed overload.
  --package-root <dir>      Enrich query metadata from real DPK/DPROJ declarations.
  --query-timeout-ms <n>    Per-request semantic budget. Default: 10000.
  --no-source-scan          Disable source fallback (default when JDI indexes are supplied).
  --allow-source-scan       Allow an explicit source fallback when supported.
  diagnose, legacy-refs, model-migration, and compiler-log emit additive schema 2.2 JSON.
  type-info and symbol-origin emit additive schema 2.3 JSON.
  Query commands build and reuse <index>.qidx sidecars using JDI path, size,
  timestamp, and fingerprint validation.
  These commands are read-only and never modify Delphi sources.

ANALYSIS OUTPUT
  Project JSON preserves legacy schemaVersion=1 fields and adds schema_version="2.1".
  Existing status, confidence, and reasons fields are never renamed or removed.
  V2 adds project, uses, references, symbols, inheritance, dependencies, diagnostics,
  preprocessor metadata, calls, assignments, and removal recommendations.

SAFE USES REMOVAL
  Automatic removal is allowed only when all conditions hold:
    status     = UNUSED
    confidence = HIGH
    reasons    = ["no_references"]
  Read recommendations.safeRemoveUses for approved candidates. Every USED, UNKNOWN,
  low-confidence, ambiguous, incompletely indexed, or side-effect dependency appears
  in recommendations.blockedRemovals.

PREPROCESSING
  The selected DPROJ configuration/platform controls DCC_UnitSearchPath,
  DCC_IncludePath, DCC_Define, and DCC_Namespace. Supported Delphi directives include
  IFDEF, IFNDEF, IF DEFINED, ELSEIF, ELSE, ENDIF, IFEND, DEFINE, UNDEF, I, and INCLUDE.
  Inactive source is masked without changing byte offsets or line endings.

PERSISTENT INDEX
  The JDI stores unit exports, available signatures, return types, inheritance,
  source paths, package metadata, dependencies, side effects, and catalog version.
  A bootstrap RTL/VCL/WinAPI/V600 catalog is embedded in the executable. Explicit
  indexes override matching embedded units while preserving known side-effect flags.

SEMANTIC SAFETY
  The tool does not invent owners, signatures, package names, overloads, or mappings.
  Incomplete or ambiguous information is returned as unresolved/unknown with stable
  machine-readable diagnostics. Analysis never modifies Delphi sources.

EXIT CODES
  0  Command executed and JSON/report produced. Semantic warnings may be in diagnostics.
  2  Invalid command line or blocking validation/I/O/index error.

VALIDATION SEVERITY
  Default warnings: no_unit_declaration, recoverable incomplete_unit,
  duplicate_unit_source, source_unit_not_indexed.
  Default errors: missing source root, file read failure, unrecoverable parse failure,
  duplicate symbol, malformed/inconsistent JDI, missing seed target,
  seed signature incompatibility, and missing V500/V600 index.
  Use --validation-policy <file> with code=warning or code=error lines to override.
  Blocking errors are returned in validation.has_blocking_errors and blocking_errors.

EXAMPLES
  DelphiAstTool.exe analyze --dproj Plugin.dproj --config Release --platform Win32 --index v600.jdi --format json
  DelphiAstTool.exe --project Plugin.dpk --output Plugin.ast.json
  DelphiAstTool.exe symbol --name RegCsProc --index v600.jdi --format json
  DelphiAstTool.exe expression --dproj Plugin.dproj --file UXPTab.pas --line 684 --index v600.jdi --format json
  DelphiAstTool.exe compare-symbol --left-index v500.jdi --right-index v600.jdi --symbol UCSTheme --format json
  DelphiAstTool.exe compare-units --left-index v500.jdi --right-index v600.jdi --seed uses_mapping_seed.json --output uses_v500_to_v600.json --format json
)";
}

bool isQueryCommand(std::string_view command) {
  return command == "exports" || command == "symbol" || command == "expression" ||
         command == "hierarchy" || command == "unit-info" || command == "compare-symbol" ||
         command == "diagnose" || command == "legacy-refs" || command == "model-migration" ||
          command == "compiler-log" || command == "type-info" || command == "symbol-origin" ||
          command == "semantic-rule-match" || command == "batch-diagnose" || command == "build-query-index";
}

int runQuery(std::string_view command, int argc, char** argv) {
  std::vector<std::filesystem::path> indexes, leftIndexes, rightIndexes, packageRoots;
  std::filesystem::path file, input, output, unitMap;
  std::filesystem::path dproj;
  std::string unit, legacyUnit, name, typeName, kind, qualifiedName, className, errorCode, rule;
  std::string configuration{"Release"}, platform{"Win32"};
  std::size_t line{};
  std::size_t queryTimeoutMs{10000};
  bool includeInherited = false, includeOverloads = false, allowSourceScan = false;
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (i == 2 && !argument.empty() && argument[0] != '-' &&
        (command == "type-info" || command == "symbol-origin")) {
      if (command == "type-info") typeName = argument; else name = argument;
      continue;
    }
    if (argument == "--include-inherited") { includeInherited = true; continue; }
    if (argument == "--include-overloads") { includeOverloads = true; continue; }
    if (argument == "--allow-source-scan") { allowSourceScan = true; continue; }
    if (argument == "--no-source-scan") { allowSourceScan = false; continue; }
    if (argument == "--format") {
      if (++i >= argc || std::string(argv[i]) != "json") { std::cerr << "Only --format json is supported\n"; return 2; }
      continue;
    }
    if (i + 1 >= argc) { std::cerr << "Missing value for " << argument << '\n'; return 2; }
    const std::string value = argv[++i];
    if (argument == "--index") indexes.emplace_back(value);
    else if (argument == "--v500-index") leftIndexes.emplace_back(value);
    else if (argument == "--v600-index") indexes.emplace_back(value);
    else if (argument == "--package-root") packageRoots.emplace_back(value);
    else if (argument == "--left-index") leftIndexes.emplace_back(value);
    else if (argument == "--right-index") rightIndexes.emplace_back(value);
    else if (argument == "--file") file = value;
    else if (argument == "--input") input = value;
    else if (argument == "--output") output = value;
    else if (argument == "--unit-map") unitMap = value;
    else if (argument == "--unit") unit = value;
    else if (argument == "--legacy-unit") legacyUnit = value;
    else if (argument == "--name" || argument == "--symbol") name = value;
    else if (argument == "--type") typeName = value;
    else if (argument == "--kind") kind = value;
    else if (argument == "--qualified-name") qualifiedName = value;
    else if (argument == "--class") className = value;
    else if (argument == "--error-code") errorCode = value;
    else if (argument == "--rule") rule = value;
    else if (argument == "--line") {
      try { line = std::stoull(value); } catch (...) { std::cerr << "Invalid line number\n"; return 2; }
    } else if (argument == "--dproj") dproj = value;
    else if (argument == "--query-timeout-ms") {
      try { queryTimeoutMs = std::stoull(value); } catch (...) { std::cerr << "Invalid query timeout\n"; return 2; }
    }
    else if (argument == "--config") configuration = value;
    else if (argument == "--platform") platform = value;
    else { std::cerr << "Unknown " << command << " option: " << argument << '\n'; return 2; }
  }
  (void)allowSourceScan;
  try {
    if (command == "build-query-index") {
      indexes.insert(indexes.end(), leftIndexes.begin(), leftIndexes.end());
      indexes.insert(indexes.end(), rightIndexes.begin(), rightIndexes.end());
      if (indexes.empty()) { std::cerr << "build-query-index requires --index, --v500-index, or --v600-index\n"; return 2; }
      std::cout << "{\"schema_version\":\"2.3\",\"indexes\":[";
      for (std::size_t i = 0; i < indexes.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << jdelphiast::buildQueryIndex(indexes[i]);
      }
      std::cout << "]}\n";
      return 0;
    }
    if (command == "batch-diagnose") {
      if (input.empty() || output.empty() || indexes.empty()) {
        std::cerr << "batch-diagnose requires --input, --index and --output\n";
        return 2;
      }
      jdelphiast::QueryPerformance performance;
      performance.startupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - processStarted).count();
      const auto loadStarted = std::chrono::steady_clock::now();
      const auto terms = batchDiagnosticTerms(input);
      jdelphiast::QueryPerformance rightPerformance, leftPerformance;
      auto rightFuture = std::async(std::launch::async, [&] { return loadFilteredIndexes(indexes, terms, &rightPerformance); });
      auto leftFuture = std::async(std::launch::async, [&] { return loadFilteredIndexes(leftIndexes, terms, &leftPerformance); });
      const auto mapStarted = std::chrono::steady_clock::now();
      if (!unitMap.empty()) (void)jdelphiast::loadUnitMappingCatalog(unitMap);
      performance.unitMapLoadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - mapStarted).count();
      auto right = rightFuture.get();
      auto left = leftFuture.get();
      performance.indexLoadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - loadStarted).count();
      performance.cacheHit = false;
      performance.v500IndexLoadMs = leftPerformance.indexLoadMs;
      performance.v600IndexLoadMs = rightPerformance.indexLoadMs;
      performance.cacheLoadMs = leftPerformance.cacheLoadMs + rightPerformance.cacheLoadMs;
      performance.cacheBuildMs = leftPerformance.cacheBuildMs + rightPerformance.cacheBuildMs;
      performance.cacheHits = (leftPerformance.cacheHit ? 1 : 0) + (rightPerformance.cacheHit ? 1 : 0);
      performance.cacheStatus = performance.cacheHits == 2 ? "hit" :
          (leftPerformance.cacheStatus == "rebuilt" || rightPerformance.cacheStatus == "rebuilt" ? "rebuilt" : "miss");
      const auto result = jdelphiast::batchDiagnoseJson(input, right, left, unitMap, queryTimeoutMs, performance);
      writeAtomic(output, result + "\n");
      std::cout << result << '\n';
      return 0;
    }
    if (command == "compiler-log") {
      if (input.empty()) { std::cerr << "compiler-log requires --input\n"; return 2; }
      jdelphiast::QueryPerformance performance;
      performance.startupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - processStarted).count();
      performance.timeoutMs = queryTimeoutMs;
      std::cout << jdelphiast::compilerLogJson(input, performance) << '\n';
      return 0;
    }
    if (command == "semantic-rule-match") {
      if (rule.empty() || file.empty() || line == 0) {
        std::cerr << "semantic-rule-match requires --rule, --file and --line\n";
        return 2;
      }
      jdelphiast::QueryPerformance performance;
      performance.startupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - processStarted).count();
      performance.timeoutMs = queryTimeoutMs;
      std::cout << jdelphiast::semanticRuleMatchJson(rule, file, line, performance) << '\n';
      return 0;
    }
    if (command == "type-info" || command == "symbol-origin") {
      if ((command == "type-info" ? typeName.empty() : name.empty()) || (indexes.empty() && leftIndexes.empty())) {
        std::cerr << command << " requires " << (command == "type-info" ? "--type" : "--name") << " and an index\n";
        return 2;
      }
      jdelphiast::QueryPerformance performance;
      performance.startupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - processStarted).count();
      performance.timeoutMs = queryTimeoutMs;
      auto queryIndexes = indexes;
      if (command == "symbol-origin") queryIndexes.insert(queryIndexes.end(), leftIndexes.begin(), leftIndexes.end());
      auto loaded = loadFilteredIndexes(queryIndexes, {command == "type-info" ? typeName : name}, &performance);
      const auto packageStarted = std::chrono::steady_clock::now();
      jdelphiast::enrichPackageMetadata(loaded, packageRoots);
      performance.packageEnrichmentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - packageStarted).count();
      if (performance.indexLoadMs + performance.packageEnrichmentMs >= static_cast<long long>(queryTimeoutMs)) {
        std::cout << queryTimeoutJson("2.3", command, command == "type-info" ? typeName : name, performance) << '\n';
        return 0;
      }
      if (command == "type-info")
        std::cout << jdelphiast::typeInfoJson(typeName, loaded, includeInherited, includeOverloads, performance) << '\n';
      else std::cout << jdelphiast::symbolOriginJson(name, loaded, performance) << '\n';
      return 0;
    }
    if (command == "compare-symbol") {
      if (name.empty() || leftIndexes.empty() || rightIndexes.empty()) { std::cerr << "compare-symbol requires --symbol, --left-index and --right-index\n"; return 2; }
      std::vector<jdelphiast::IndexedUnit> left, right;
      for (const auto& path : leftIndexes) { auto loaded = jdelphiast::loadSymbolIndex(path); left.insert(left.end(), loaded.begin(), loaded.end()); }
      for (const auto& path : rightIndexes) { auto loaded = jdelphiast::loadSymbolIndex(path); right.insert(right.end(), loaded.begin(), loaded.end()); }
      std::cout << jdelphiast::compareSymbolJson(name, left, right) << '\n';
      return 0;
    }
    if (command == "legacy-refs" || command == "model-migration") {
      if (file.empty() || leftIndexes.empty() || rightIndexes.empty() ||
          (command == "legacy-refs" ? legacyUnit.empty() : name.empty())) {
        std::cerr << command << " requires --file, --left-index, --right-index and "
                  << (command == "legacy-refs" ? "--legacy-unit\n" : "--symbol\n");
        return 2;
      }
      jdelphiast::QueryPerformance performance;
      performance.startupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - processStarted).count();
      performance.timeoutMs = queryTimeoutMs;
      const auto loadStarted = std::chrono::steady_clock::now();
      auto terms = diagnosticTerms(file, command == "legacy-refs" ? legacyUnit : name);
      if (command == "model-migration") {
        const std::vector<std::string> modelTerms = {"EnableOnChange", "FieldValues", "GetCsField", "CSSeek", "CSModify", "CSResetCampi"};
        terms.insert(terms.end(), modelTerms.begin(), modelTerms.end());
      }
      jdelphiast::QueryPerformance leftPerformance, rightPerformance;
      leftPerformance.timeoutMs = queryTimeoutMs;
      rightPerformance.timeoutMs = queryTimeoutMs;
      auto leftFuture = std::async(std::launch::async, [&] { return loadFilteredIndexes(leftIndexes, terms, &leftPerformance); });
      auto rightFuture = std::async(std::launch::async, [&] { return loadFilteredIndexes(rightIndexes, terms, &rightPerformance); });
      auto left = leftFuture.get();
      auto right = rightFuture.get();
      performance.indexLoadMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - loadStarted).count();
      performance.indexOpenMs = leftPerformance.indexOpenMs + rightPerformance.indexOpenMs;
      performance.indexParseMs = leftPerformance.indexParseMs + rightPerformance.indexParseMs;
      performance.cacheLoadMs = leftPerformance.cacheLoadMs + rightPerformance.cacheLoadMs;
      performance.cacheBuildMs = leftPerformance.cacheBuildMs + rightPerformance.cacheBuildMs;
      performance.cacheHit = leftPerformance.cacheHit && rightPerformance.cacheHit;
      performance.cacheStatus = leftPerformance.cacheStatus == "rebuilt" || rightPerformance.cacheStatus == "rebuilt"
          ? "rebuilt" : performance.cacheHit ? "hit" : "miss";
      if (performance.indexLoadMs >= static_cast<long long>(queryTimeoutMs)) {
        std::cout << queryTimeoutJson("2.2", command, command == "legacy-refs" ? legacyUnit : name, performance) << '\n';
        return 0;
      }
      if (command == "legacy-refs")
        std::cout << jdelphiast::legacyReferencesJson(file, legacyUnit, left, right, unitMap, performance) << '\n';
      else std::cout << jdelphiast::modelMigrationJson(file, name, left, right, unitMap, performance) << '\n';
      return 0;
    }
    if (command == "diagnose") {
      if (file.empty() || line == 0 || errorCode.empty() || indexes.empty()) {
        std::cerr << "diagnose requires --file, --line, --error-code and --index\n";
        return 2;
      }
      const auto terms = diagnosticTerms(file, name, line);
      jdelphiast::QueryPerformance performance;
      performance.timeoutMs = queryTimeoutMs;
      const auto loadStarted = std::chrono::steady_clock::now();
      jdelphiast::QueryPerformance leftPerformance, rightPerformance;
      leftPerformance.timeoutMs = queryTimeoutMs;
      rightPerformance.timeoutMs = queryTimeoutMs;
      auto leftFuture = std::async(std::launch::async, [&] { return loadFilteredIndexes(leftIndexes, terms, &leftPerformance); });
      auto rightFuture = std::async(std::launch::async, [&] { return loadFilteredIndexes(indexes, terms, &rightPerformance); });
      auto left = leftFuture.get();
      auto right = rightFuture.get();
      performance.indexLoadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - loadStarted).count();
      performance.indexOpenMs = leftPerformance.indexOpenMs + rightPerformance.indexOpenMs;
      performance.indexParseMs = leftPerformance.indexParseMs + rightPerformance.indexParseMs;
      performance.cacheLoadMs = leftPerformance.cacheLoadMs + rightPerformance.cacheLoadMs;
      performance.cacheBuildMs = leftPerformance.cacheBuildMs + rightPerformance.cacheBuildMs;
      performance.cacheHit = leftPerformance.cacheHit && rightPerformance.cacheHit;
      performance.cacheStatus = leftPerformance.cacheStatus == "rebuilt" || rightPerformance.cacheStatus == "rebuilt"
          ? "rebuilt" : performance.cacheHit ? "hit" : "miss";
      const auto packageStarted = std::chrono::steady_clock::now();
      jdelphiast::enrichPackageMetadata(right, packageRoots);
      performance.packageEnrichmentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - packageStarted).count();
      if (performance.indexLoadMs + performance.packageEnrichmentMs >= static_cast<long long>(queryTimeoutMs)) {
        std::cout << queryTimeoutJson("2.2", command, name, performance) << '\n';
        return 0;
      }
      if (!dproj.empty()) {
        jdelphiast::ProjectOptions projectOptions;
        projectOptions.dprojFile = dproj;
        projectOptions.configuration = configuration;
        projectOptions.platform = platform;
        const auto project = jdelphiast::loadPackage(packageFromDproj(dproj), projectOptions);
        jdelphiast::mergeProjectSymbols(right, project.analyzer.analyze());
      }
      jdelphiast::DiagnoseOptions options;
      options.file = file; options.dproj = dproj; options.unitMap = unitMap;
      options.configuration = configuration; options.platform = platform;
      options.errorCode = errorCode; options.symbol = name; options.line = line; options.performance = performance;
      options.timeoutMs = queryTimeoutMs;
      std::cout << jdelphiast::diagnoseJson(options, right, left) << '\n';
      return 0;
    }
    jdelphiast::QueryPerformance performance;
    performance.startupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - processStarted).count();
    performance.timeoutMs = queryTimeoutMs;
    auto loaded = (command == "unit-info" ? loadFilteredIndexes(indexes, {unit}, &performance) :
                   command == "symbol" ? loadFilteredIndexes(indexes, {name}, &performance) :
                   loadIndexes(indexes, &performance));
    jdelphiast::enrichPackageMetadata(loaded, packageRoots);
    if ((command == "unit-info" || command == "symbol") &&
        performance.indexLoadMs + performance.packageEnrichmentMs >= static_cast<long long>(queryTimeoutMs)) {
      std::cout << queryTimeoutJson("2.1", command, command == "unit-info" ? unit : name, performance) << '\n';
      return 0;
    }
    if (!dproj.empty() && (command == "expression" || command == "hierarchy")) {
      jdelphiast::ProjectOptions options;
      options.dprojFile = dproj;
      options.configuration = configuration;
      options.platform = platform;
      options.indexFiles = indexes;
      const auto project = jdelphiast::loadPackage(packageFromDproj(dproj), options);
      const auto analysis = project.analyzer.analyze();
      for (const auto& analyzed : analysis.units) {
        if (analyzed.ast.indexOnly) continue;
        jdelphiast::IndexedUnit projectUnit;
        projectUnit.name = analyzed.ast.name;
        projectUnit.sourceFile = analyzed.ast.file;
        projectUnit.indexVersion = "project";
        projectUnit.declarations = analyzed.ast.declarations;
        projectUnit.inheritance = analyzed.ast.inheritance;
        for (const auto& symbol : analyzed.ast.exports) projectUnit.symbols.push_back(symbol.name);
        const auto existing = std::find_if(loaded.begin(), loaded.end(), [&](const auto& candidate) {
          return candidate.name == projectUnit.name;
        });
        if (existing == loaded.end()) loaded.push_back(std::move(projectUnit)); else *existing = std::move(projectUnit);
      }
    }
    if (command == "exports") {
      if (file.empty() && unit.empty()) { std::cerr << "exports requires --unit or --file\n"; return 2; }
      std::cout << jdelphiast::exportsJson(file.empty() ? std::filesystem::path(unit) : file, loaded) << '\n';
    } else if (command == "symbol") {
      if (name.empty()) { std::cerr << "symbol requires --name\n"; return 2; }
      std::cout << jdelphiast::symbolJson({name, unit, kind, qualifiedName}, loaded, performance) << '\n';
    } else if (command == "unit-info") {
      if (unit.empty()) { std::cerr << "unit-info requires --unit\n"; return 2; }
      std::cout << jdelphiast::unitInfoJson(unit, loaded, performance) << '\n';
    } else if (command == "expression") {
      if (file.empty() || line == 0) { std::cerr << "expression requires --file and --line\n"; return 2; }
      std::cout << jdelphiast::expressionJson(file, line, loaded) << '\n';
    } else if (command == "hierarchy") {
      if (className.empty()) { std::cerr << "hierarchy requires --class and --file or --dproj\n"; return 2; }
      if (file.empty() && !dproj.empty()) {
        jdelphiast::ProjectOptions options;
        options.dprojFile = dproj;
        options.configuration = configuration;
        options.platform = platform;
        options.indexFiles = indexes;
        const auto project = jdelphiast::loadPackage(packageFromDproj(dproj), options);
        for (const auto& candidate : project.sourceFiles) {
          const auto source = [&] { std::ifstream input(candidate, std::ios::binary); return std::string(std::istreambuf_iterator<char>(input), {}); }();
          const auto ast = jdelphiast::parseUnit(candidate, source);
          if (std::any_of(ast.inheritance.begin(), ast.inheritance.end(), [&](const auto& relation) {
                auto left = relation.type;
                auto right = className;
                std::transform(left.begin(), left.end(), left.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(right.begin(), right.end(), right.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return left == right;
              })) { file = candidate; break; }
        }
      }
      if (file.empty()) { std::cerr << "hierarchy could not locate the requested class source\n"; return 2; }
      std::cout << jdelphiast::hierarchyJson(file, className, loaded) << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cout << "{\"schema_version\":\"2.1\",\"diagnostics\":[{\"code\":\"query_execution_failed\","
                 "\"severity\":\"error\",\"message\":\"";
    for (const char c : std::string(error.what())) {
      if (c == '\\' || c == '"') std::cout << '\\';
      std::cout << c;
    }
    std::cout << "\"}]}\n";
    return 0;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && (std::string(argv[1]) == "/?" || std::string(argv[1]) == "--help" ||
                   std::string(argv[1]) == "-h" || std::string(argv[1]) == "help")) {
    printHelp();
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "index") {
    std::vector<std::filesystem::path> sources;
    std::filesystem::path indexOutput;
    std::string indexVersion{"unknown"};
    std::string origin;
    std::vector<std::filesystem::path> sourceRoots, packageRoots;
    std::filesystem::path validationPolicy;
    bool jsonOutput = false;
    for (int i = 2; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--format") {
        if (++i >= argc || std::string(argv[i]) != "json") return 2;
        jsonOutput = true;
        continue;
      }
      if ((argument == "--source" || argument == "--output" || argument == "--version" || argument == "--origin" ||
           argument == "--source-root" || argument == "--package-root" || argument == "--validation-policy") && i + 1 >= argc) {
        std::cerr << "Missing value for " << argument << '\n';
        return 2;
      }
      if (argument == "--source") sources.emplace_back(argv[++i]);
      else if (argument == "--output") indexOutput = argv[++i];
      else if (argument == "--version") indexVersion = argv[++i];
      else if (argument == "--origin") origin = argv[++i];
      else if (argument == "--source-root") sourceRoots.emplace_back(argv[++i]);
      else if (argument == "--package-root") packageRoots.emplace_back(argv[++i]);
      else if (argument == "--validation-policy") validationPolicy = argv[++i];
      else { std::cerr << "Unknown index option: " << argument << '\n'; return 2; }
    }
    if ((sources.empty() && sourceRoots.empty()) || indexOutput.empty()) {
      std::cerr << "Usage: DelphiAstTool index (--source path | --source-root path) [...] --output index.jdi\n";
      return 2;
    }
    try {
      jdelphiast::IndexBuildOptions options;
      options.version = indexVersion;
      options.origin = origin;
      options.sourceRoots = sourceRoots;
      options.packageRoots = packageRoots;
      options.validationPolicy = validationPolicy;
      const auto result = jdelphiast::buildSymbolIndex(sources, options);
      if (jsonOutput) std::cout << jdelphiast::indexBuildResultJson(result, indexOutput) << '\n';
      if (result.hasBlockingErrors) {
        std::error_code error;
        std::filesystem::remove(indexOutput, error);
        return 2;
      }
      writeAtomic(indexOutput, result.content);
      if (!jsonOutput) std::cout << "Wrote " << indexOutput.string() << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cerr << "error: " << error.what() << '\n';
      return 2;
    }
  }
  if (argc > 1 && std::string(argv[1]) == "compare-units") {
    jdelphiast::UnitMapOptions options;
    std::filesystem::path output, csv, html;
    for (int i = 2; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--include-unmapped") { options.includeUnmapped = true; continue; }
      if (argument == "--include-ambiguous") { options.includeAmbiguous = true; continue; }
      if (argument == "--include-symbol-details") { options.includeSymbolDetails = true; continue; }
      if (argument == "--format") { if (++i >= argc || std::string(argv[i]) != "json") return 2; continue; }
      if (++i >= argc) { std::cerr << "Missing value for " << argument << '\n'; return 2; }
      const std::string value = argv[i];
      if (argument == "--left-index") options.leftIndex = value;
      else if (argument == "--right-index") options.rightIndex = value;
      else if (argument == "--output") output = value;
      else if (argument == "--seed") options.seedFile = value;
      else if (argument == "--csv") csv = value;
      else if (argument == "--html") html = value;
      else if (argument == "--minimum-confidence") options.minimumConfidence = value;
      else if (argument == "--validation-policy") options.validationPolicy = value;
      else { std::cerr << "Unknown compare-units option: " << argument << '\n'; return 2; }
    }
    if (options.leftIndex.empty() || options.rightIndex.empty() || output.empty()) {
      std::cerr << "compare-units requires --left-index, --right-index and --output\n"; return 2;
    }
    if (options.minimumConfidence != "low" && options.minimumConfidence != "medium" && options.minimumConfidence != "high") {
      std::cerr << "--minimum-confidence must be low, medium, or high\n";
      return 2;
    }
    try {
      const auto left = jdelphiast::loadSymbolIndexValidated(options.leftIndex);
      const auto right = jdelphiast::loadSymbolIndexValidated(options.rightIndex);
      options.validationDiagnostics = left.diagnostics;
      options.validationDiagnostics.insert(options.validationDiagnostics.end(), right.diagnostics.begin(), right.diagnostics.end());
      const auto versionMismatch = [](const std::vector<jdelphiast::IndexedUnit>& units, std::string_view expected) {
        return std::any_of(units.begin(), units.end(), [&](const auto& unit) {
          return unit.indexVersion != "unknown" && unit.indexVersion != expected;
        });
      };
      if (versionMismatch(left.units, "v500")) options.validationDiagnostics.push_back(
          {"index_version_mismatch", "error", "Left catalog is not consistently V500.", options.leftIndex, {}});
      if (versionMismatch(right.units, "v600")) options.validationDiagnostics.push_back(
          {"index_version_mismatch", "error", "Right catalog is not consistently V600.", options.rightIndex, {}});
      const auto leftUnits = left.units.empty() && !left.hasBlockingErrors ? jdelphiast::loadSymbolIndex(options.leftIndex) : left.units;
      const auto rightUnits = right.units.empty() && !right.hasBlockingErrors ? jdelphiast::loadSymbolIndex(options.rightIndex) : right.units;
      const auto result = jdelphiast::compareUnits(options, leftUnits, rightUnits, timestamp());
      if (result.hasBlockingErrors) {
        std::error_code error;
        std::filesystem::remove(output, error);
        if (!csv.empty()) std::filesystem::remove(csv, error);
        if (!html.empty()) std::filesystem::remove(html, error);
        std::cout << result.json << '\n';
        return 2;
      }
      writeAtomic(output, result.json + "\n");
      if (!csv.empty()) writeAtomic(csv, result.csv);
      if (!html.empty()) writeAtomic(html, result.html);
      std::cout << result.json << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::error_code removeError;
      std::filesystem::remove(output, removeError);
      if (!csv.empty()) std::filesystem::remove(csv, removeError);
      if (!html.empty()) std::filesystem::remove(html, removeError);
      std::string code = "compare_units_failed";
      const std::string message = error.what();
      if (message.find("Seed mapping file") != std::string::npos) code = "seed_missing";
      else if (message.find("seed mapping schema") != std::string::npos) code = "seed_schema_invalid";
      else if (message.find("seed mapping") != std::string::npos || message.find("seed confidence") != std::string::npos)
        code = "seed_parse_failed";
      std::cout << "{\"schema_version\":\"1.0\",\"validation\":{\"has_blocking_errors\":true,\"error_count\":1,\"warning_count\":0},"
                   "\"mappings\":[],\"diagnostics\":[{\"code\":\"" << code
                << "\",\"severity\":\"error\",\"message\":\"compare-units validation failed\"}],"
                   "\"blocking_errors\":[{\"code\":\"" << code << "\",\"message\":\"compare-units validation failed\"}]}\n";
      std::cerr << "error: " << error.what() << '\n';
      return 2;
    }
  }
  if (argc > 1 && isQueryCommand(argv[1])) return runQuery(argv[1], argc, argv);
  const bool explicitAnalyze = argc > 1 && std::string(argv[1]) == "analyze";
  bool json = false;
  int fileCount = 0;
  int sourceFileCount = 0;
  jdelphiast::Analyzer analyzer;
  jdelphiast::ProjectOptions projectOptions;
  std::filesystem::path package;
  std::filesystem::path outputPath;
  for (int i = explicitAnalyze ? 2 : 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--json") {
      json = true;
      continue;
    }
    if (argument == "--format") {
      if (++i >= argc || std::string(argv[i]) != "json") { std::cerr << "Only --format json is supported\n"; return 2; }
      json = true;
      continue;
    }
    if ((argument == "--index" || argument == "--search-path" || argument == "--include-path" ||
         argument == "--project" || argument == "--output" || argument == "--config" ||
         argument == "--platform" || argument == "--dproj" || argument == "--unit-map") && i + 1 >= argc) {
      std::cerr << "Missing value for " << argument << '\n';
      return 2;
    }
    if (argument == "--index") {
      projectOptions.indexFiles.emplace_back(argv[++i]);
      continue;
    }
    if (argument == "--search-path") {
      projectOptions.searchPaths.emplace_back(argv[++i]);
      continue;
    }
    if (argument == "--include-path") {
      projectOptions.includePaths.emplace_back(argv[++i]);
      continue;
    }
    if (argument == "--config") {
      projectOptions.configuration = argv[++i];
      continue;
    }
    if (argument == "--platform") {
      projectOptions.platform = argv[++i];
      continue;
    }
    if (argument == "--dproj") {
      projectOptions.dprojFile = argv[++i];
      continue;
    }
    if (argument == "--unit-map") {
      projectOptions.unitMappingCatalog = argv[++i];
      continue;
    }
    if (argument == "--project") {
      if (!package.empty()) {
        std::cerr << "--project can be specified only once\n";
        return 2;
      }
      package = argv[++i];
      ++fileCount;
      continue;
    }
    if (argument == "--output") {
      outputPath = argv[++i];
      continue;
    }
    auto extension = std::filesystem::path(argument).extension().string();
    for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension == ".dpk") {
      if (!package.empty()) {
        std::cerr << "Only one package can be analyzed per invocation\n";
        return 2;
      }
      package = argument;
      ++fileCount;
      continue;
    }
    std::ifstream input(argument, std::ios::binary);
    if (!input) {
      std::cerr << "Cannot read " << argument << '\n';
      return 2;
    }
    analyzer.addSource(argument, {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()});
    ++fileCount;
    ++sourceFileCount;
  }
  if (explicitAnalyze && package.empty() && !projectOptions.dprojFile.empty()) {
    package = packageFromDproj(projectOptions.dprojFile);
    ++fileCount;
  }
  if (fileCount == 0) {
    std::cerr << "Usage: DelphiAstTool --project package.dpk --output package.ast.json "
                 "[--config Release] [--platform Win32] [--dproj file.dproj] "
                 "[--index file.jdi] [--search-path dir] [--include-path dir]\n"
                 "       DelphiAstTool [--json] unit1.pas [unit2.pas ...]\n";
    return 2;
  }
  if ((!explicitAnalyze && !package.empty() && outputPath.empty()) || (package.empty() && !outputPath.empty())) {
    std::cerr << "--project and --output must be specified together\n";
    return 2;
  }
  try {
    if (!package.empty()) {
      if (sourceFileCount != 0) {
        std::cerr << "Cannot mix a package with explicit Pascal source files\n";
        return 2;
      }
      std::vector<std::filesystem::path> defaultIndexes;
      const auto executableDir = std::filesystem::absolute(argv[0]).parent_path();
      const auto installedIndex = executableDir / "indexes" / "delphi-v600.jdi";
      const auto sourceIndex = std::filesystem::current_path() / "indexes" / "delphi-v600.jdi";
      if (std::filesystem::exists(installedIndex)) defaultIndexes.push_back(installedIndex);
      else if (std::filesystem::exists(sourceIndex)) defaultIndexes.push_back(sourceIndex);
      for (const auto& index : defaultIndexes)
        if (std::find(projectOptions.indexFiles.begin(), projectOptions.indexFiles.end(), index) ==
            projectOptions.indexFiles.end()) projectOptions.indexFiles.insert(projectOptions.indexFiles.begin(), index);
      auto loaded = jdelphiast::loadPackage(package, std::move(projectOptions));
      for (const auto& diagnostic : loaded.diagnostics) std::cerr << "warning: " << diagnostic << '\n';
      const auto result = loaded.analyzer.analyze();
      const auto report = outputPath.empty() && !explicitAnalyze
          ? (json ? jdelphiast::toJson(result) : jdelphiast::toText(result))
          : jdelphiast::toProjectJson(result, package.stem().string(),
                                      std::filesystem::absolute(package).parent_path(), timestamp());
      if (outputPath.empty()) std::cout << report;
      else {
        if (!outputPath.parent_path().empty()) std::filesystem::create_directories(outputPath.parent_path());
        const auto temporary = outputPath.string() + ".tmp";
        {
          std::ofstream output(temporary, std::ios::binary);
          if (!output) throw std::runtime_error("Cannot write " + temporary);
          output << report << '\n';
        }
        std::error_code error;
        std::filesystem::remove(outputPath, error);
        std::filesystem::rename(temporary, outputPath);
        std::cout << "Wrote " << outputPath.string() << '\n';
      }
    } else {
      for (const auto& index : projectOptions.indexFiles)
        for (auto unit : jdelphiast::loadSymbolIndex(index)) analyzer.addIndexedUnit(std::move(unit));
      const auto result = analyzer.analyze();
      std::cout << (json ? jdelphiast::toJson(result) : jdelphiast::toText(result));
    }
  } catch (const std::exception& error) {
    if (explicitAnalyze && json) {
      std::cout << "{\"schemaVersion\":1,\"schema_version\":\"2.1\",\"project\":{},\"units\":[],"
                   "\"uses\":[],\"references\":[],\"symbols\":[],\"inheritance\":[],\"dependencies\":[],"
                   "\"diagnostics\":[{\"code\":\"analysis_failed\",\"severity\":\"error\",\"message\":\"";
      for (const char c : std::string(error.what())) { if (c == '\\' || c == '"') std::cout << '\\'; std::cout << c; }
      std::cout << "\"}]}\n";
      return 0;
    }
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
