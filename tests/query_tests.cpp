#include "jdelphiast/project.hpp"
#include "jdelphiast/queries.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
  if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}

int main() {
  const auto fixtures = std::filesystem::path(JDELPHIAST_FIXTURE_DIR);
  const auto left = jdelphiast::loadSymbolIndex(fixtures / "v500.jdi");
  const auto right = jdelphiast::loadSymbolIndex(fixtures / "v600.jdi");

  const auto comparison = jdelphiast::compareSymbolJson("UCSTheme", left, right);
  require(comparison.find("partial_compatible") != std::string::npos, "UCSTheme is partially compatible");
  require(comparison.find("AppStyle") != std::string::npos && comparison.find("BitCount") != std::string::npos,
          "missing legacy exports are reported");

  const auto symbol = jdelphiast::symbolJson({"RegCsProc", {}, {}, {}}, right);
  require(symbol.find("procedure RegCsProc") != std::string::npos, "RegCsProc signature is returned");

  const auto expression = jdelphiast::expressionJson(fixtures / "ExpressionSample.pas", 14, right);
  require(expression.find("\"resolved\":true") != std::string::npos &&
              expression.find("\"name\":\"TFDParam\"") != std::string::npos,
          "FindParam expression resolves to TFDParam");
  const auto missingExpression = jdelphiast::expressionJson(fixtures / "ExpressionSample.pas", 1, right);
  require(missingExpression.find("\"resolved\":false") != std::string::npos,
          "non-call expression remains unresolved");

  const auto hierarchy = jdelphiast::hierarchyJson(fixtures / "HierarchySample.pas", "TOExtArtControCamp", right);
  require(hierarchy.find("override_signature_incompatible") != std::string::npos,
          "incompatible constructor override is diagnosed");

  const auto info = jdelphiast::unitInfoJson("UMarketing", right);
  require(info.find("K:/V0600/Modules/M_Mrk/UMarketing.pas") != std::string::npos,
          "UMarketing source path is returned");
  require(info.find("package_metadata_not_available") != std::string::npos,
          "missing package metadata is explicit");

  const auto exports = jdelphiast::exportsJson("CSControls.Theme", right);
  require(exports.find("CSGlobalTheme") != std::string::npos && exports.find("TCSTheme") != std::string::npos,
          "CSControls.Theme exports are returned");

  jdelphiast::IndexedUnit unrelated;
  unrelated.name = "Unrelated";
  unrelated.symbols = {"CSGlobalTheme"};
  const auto noMapping = jdelphiast::compareSymbolJson("UCSTheme", left, {unrelated});
  require(noMapping.find("legacy_symbol_unmapped") != std::string::npos,
          "single common export does not create an invented migration mapping");

  jdelphiast::UnitMapOptions mapOptions;
  mapOptions.leftIndex = fixtures / "unit-map-v500.jdi";
  mapOptions.rightIndex = fixtures / "unit-map-v600.jdi";
  mapOptions.seedFile = std::filesystem::path(JDELPHIAST_SOURCE_DIR) / "mappings" / "uses_mapping_seed.json";
  mapOptions.includeUnmapped = true;
  mapOptions.includeAmbiguous = true;
  mapOptions.includeSymbolDetails = true;
  const auto unitMap = jdelphiast::compareUnits(mapOptions,
      jdelphiast::loadSymbolIndex(mapOptions.leftIndex), jdelphiast::loadSymbolIndex(mapOptions.rightIndex),
      "2026-09-28T00:00:00");
  require(unitMap.json.find("\"v500_unit\":\"CSETables\"") != std::string::npos &&
              unitMap.json.find("\"v600_unit\":\"CSData.CSETables\"") != std::string::npos,
          "approved CSETables mapping is emitted");
  require(unitMap.json.find("\"v500_unit\":\"UCSTheme\"") != std::string::npos &&
              unitMap.json.find("\"compatibility\":\"partial_compatible\"") != std::string::npos,
          "UCSTheme is partial compatible");
  require(unitMap.json.find("\"symbol_or_unit\":\"CSMail\"") != std::string::npos &&
              unitMap.json.find("\"symbol_or_unit\":\"RNote\"") != std::string::npos,
          "semantic-only legacy APIs do not receive invented unit mappings");
  require(unitMap.json.find("Vcl.ComCtrls") != std::string::npos && unitMap.json.find("Vcl.ActnList") != std::string::npos &&
              unitMap.json.find("System.Generics.Collections") != std::string::npos &&
              unitMap.json.find("System.Win.ComObj") != std::string::npos && unitMap.json.find("Vcl.Clipbrd") != std::string::npos,
          "known namespace migrations are emitted");
  require(unitMap.json.find("\"v500_unit\":\"ComCtrls\"") != std::string::npos &&
              unitMap.json.find("\"mapping_type\":\"namespace_migration\"") != std::string::npos,
          "standard namespace migrations are classified explicitly");
  require(unitMap.json.find("\"matched_exports\":[\"CSGlobalTheme\"") != std::string::npos,
          "CSGlobalTheme is compared as a public unit export");
  require(unitMap.csv.find("v500_unit,v600_unit") != std::string::npos &&
              unitMap.html.find("Filter mappings") != std::string::npos,
          "CSV and searchable HTML reports are generated");
  jdelphiast::UnitMapOptions missingSeed = mapOptions;
  missingSeed.seedFile = fixtures / "missing-target-seed.json";
  const auto missingTarget = jdelphiast::compareUnits(missingSeed,
      jdelphiast::loadSymbolIndex(missingSeed.leftIndex), jdelphiast::loadSymbolIndex(missingSeed.rightIndex),
      "2026-09-28T00:00:00");
  require(missingTarget.json.find("seed_target_not_found") != std::string::npos &&
              missingTarget.json.find("\"confidence\":\"low\"") != std::string::npos,
          "missing seed target is diagnosed and never retains high confidence");
  require(missingTarget.json.find("\"has_blocking_errors\":true") != std::string::npos &&
              missingTarget.json.find("\"severity\":\"error\"") != std::string::npos,
          "missing seed target is a blocking validation error by default");
  missingSeed.validationPolicy = fixtures / "warning-validation.policy";
  const auto warningTarget = jdelphiast::compareUnits(missingSeed,
      jdelphiast::loadSymbolIndex(missingSeed.leftIndex), jdelphiast::loadSymbolIndex(missingSeed.rightIndex),
      "2026-09-28T00:00:00");
  require(!warningTarget.hasBlockingErrors && warningTarget.json.find("\"severity\":\"warning\"") != std::string::npos,
          "validation policy can demote a seed target diagnostic to warning");

  const auto malformed = jdelphiast::loadSymbolIndexValidated(fixtures / "malformed.jdi");
  require(malformed.hasBlockingErrors && malformed.diagnostics.front().code == "index_parse_failed",
          "malformed index is a blocking validation error");
  const auto duplicate = jdelphiast::loadSymbolIndexValidated(fixtures / "duplicate-unit.jdi");
  require(!duplicate.hasBlockingErrors && duplicate.units.size() == 2 &&
              duplicate.diagnostics.front().code == "duplicate_unit_source" &&
              duplicate.diagnostics.front().severity == "warning",
          "duplicate unit candidates are retained as a warning");
  const auto duplicateSymbol = jdelphiast::loadSymbolIndexValidated(fixtures / "duplicate-symbol.jdi");
  require(duplicateSymbol.hasBlockingErrors && duplicateSymbol.diagnostics.front().code == "duplicate_symbol",
          "duplicate exported symbol is blocking");
  const auto mixedVersion = jdelphiast::loadSymbolIndexValidated(fixtures / "mixed-version.jdi");
  require(mixedVersion.hasBlockingErrors &&
              std::any_of(mixedVersion.diagnostics.begin(), mixedVersion.diagnostics.end(), [](const auto& item) {
                return item.code == "catalog_version_inconsistent" || item.code == "catalog_origin_inconsistent";
              }), "mixed catalog version or origin is blocking");
  const auto badPackage = jdelphiast::loadSymbolIndexValidated(fixtures / "bad-package.jdi");
  require(badPackage.hasBlockingErrors && badPackage.diagnostics.front().code == "package_metadata_inconsistent",
          "inconsistent package metadata is blocking");
  const auto missingIndex = jdelphiast::loadSymbolIndexValidated(fixtures / "does-not-exist.jdi");
  require(missingIndex.hasBlockingErrors && missingIndex.diagnostics.front().code == "index_read_failed",
          "missing index is blocking");
  require(missingTarget.json.find("\"unmapped_units\":[") != std::string::npos &&
              missingTarget.json.find("\"CSETables\"") != std::string::npos,
          "missing seed target is listed as unmapped");
  jdelphiast::UnitMapOptions compactOptions = mapOptions;
  compactOptions.includeSymbolDetails = false;
  const auto compact = jdelphiast::compareUnits(compactOptions,
      jdelphiast::loadSymbolIndex(compactOptions.leftIndex), jdelphiast::loadSymbolIndex(compactOptions.rightIndex),
      "2026-09-28T00:00:00");
  require(compact.json.find("\"export_match_count\":3") != std::string::npos &&
              compact.json.find("\"matched_exports\":[]") != std::string::npos,
          "compact catalog preserves evidence counts without symbol detail arrays");
  const auto missingComparison = jdelphiast::compareSymbolJson("DefinitelyMissing", left, right);
  require(missingComparison.find("legacy_symbol_unmapped") != std::string::npos,
          "missing comparison input returns a diagnostic without crashing");
  const auto missingHierarchy = jdelphiast::hierarchyJson(fixtures / "missing.pas", "TMissing", right);
  require(missingHierarchy.find("source_file_not_found") != std::string::npos,
          "missing hierarchy source has a stable diagnostic");
  std::cout << "All query tests passed\n";
}
