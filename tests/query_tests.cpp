#include "jdelphiast/project.hpp"
#include "jdelphiast/queries.hpp"

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
  const auto missingComparison = jdelphiast::compareSymbolJson("DefinitelyMissing", left, right);
  require(missingComparison.find("legacy_symbol_unmapped") != std::string::npos,
          "missing comparison input returns a diagnostic without crashing");
  const auto missingHierarchy = jdelphiast::hierarchyJson(fixtures / "missing.pas", "TMissing", right);
  require(missingHierarchy.find("source_file_not_found") != std::string::npos,
          "missing hierarchy source has a stable diagnostic");
  std::cout << "All query tests passed\n";
}
