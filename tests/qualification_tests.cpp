#include "jdelphiast/analyzer.hpp"
#include "jdelphiast/queries.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>

void require(bool value, const char* message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

int main(int argc, char** argv) {
  using namespace jdelphiast;
  const std::string source = "\xEF\xBB\xBFunit Qualified;\r\ninterface\r\n"
    "type IWork = interface\r\nprocedure Run(X: Integer); overload;\r\n"
    "procedure Run(const X: string); overload;\r\nend;\r\n"
    "TWork = class(TObject)\r\npublic\r\nprocedure Run(X: Integer); overload;\r\n"
    "procedure Run(const X: string); overload;\r\nend;\r\n"
    "TBox<T> = class\r\nValue: T;\r\nend;\r\n"
    "TStrHelper = record helper for string\r\nfunction Size: Integer;\r\nend;\r\n"
    "implementation\r\nend.\r\n";
  auto ast = parseUnit("Qualified.pas", source);
  const auto concrete = tokenizeConcrete(source);
  require(concrete.complete && writeConcrete(concrete) == source, "CST parse/write retains BOM/CRLF bytes");
  const std::string unicode = "\xEF\xBB\xBF// citt\xC3\xA0 \xF0\x9F\x98\x80\r\nTargetCall('it''s {text}');";
  require(writeConcrete(tokenizeConcrete(unicode)) == unicode, "UTF8 non-BMP trivia/string round trip");
  const std::string ansi = "// citt\xE0\r\nTargetCall('x');";
  require(writeConcrete(tokenizeConcrete(ansi)) == ansi, "ANSI bytes round trip");
  require(jsonSourceBytes("citt\xE0") == "citt\\u00e0", "invalid UTF8 ANSI bytes use explicit JSON byte envelope");
  require(jsonSourceBytes("citt\xC3\xA0") == "citt\xC3\xA0", "valid UTF8 preserved in JSON");
  auto unclosed = tokenizeConcrete("unit X;\n{missing");
  require(!unclosed.complete && !unclosed.diagnostics.empty(), "unterminated comment diagnostic");
  require(ast.name == "Qualified", "BOM is not a unit token");
  require(ast.complete, "complete source parsed");
  const auto find = [&](const char* name, const char* kind) {
    return std::any_of(ast.declarations.begin(), ast.declarations.end(), [&](const AstDeclaration& d) {
      return d.name == name && d.kind == kind;
    });
  };
  require(find("TWork", "class"), "class kind preserved");
  require(find("TBox", "class"), "generic class header retained");
  require(find("TBox.Value", "field"), "generic class member retained");
  require(find("TStrHelper", "recordHelper"), "helper kind retained");
  require(find("TStrHelper.Size", "function"), "helper member retained");
  std::size_t overloads = 0;
  for (const auto& d : ast.declarations) if (d.name == "IWork.Run") {
    ++overloads;
    require(d.parameters.size() == 1, "parameter parsed");
    require(source.substr(d.range.begin.offset, 9) == "procedure", "byte offset maps to original BOM source");
  }
  require(overloads == 2, "two distinct interface overloads");
  auto invalid = parseUnit("Bad.pas", "unit Bad; interface procedure Run(; implementation end.");
  auto indexed = parseUnit("Indexed.pas", "unit Indexed; interface implementation procedure Run; begin CampiObj['IdArtico'] := Query.FieldByName('IdLotSer').AsInteger; end; end.");
  require(std::any_of(indexed.assignments.begin(), indexed.assignments.end(), [](const AstAssignment& assignment) {
    return assignment.left == "CampiObj['IdArtico']" && assignment.right == "Query.FieldByName('IdLotSer').AsInteger";
  }), "indexed ID producer/consumer assignment retained without inferred domain");
  require(!invalid.complete && invalid.name == "Bad", "malformed source returns incomplete without exception");
  auto incomplete = parseUnit("Half.pas", "unit Half; interface type T = class");
  require(!incomplete.complete, "unfinished unit rejected");
  auto anonymous = parseUnit("Anon.pas", "unit Anon; interface implementation procedure Outer; var P: reference to procedure; begin P := procedure begin end; end; end.");
  require(std::none_of(anonymous.declarations.begin(), anonymous.declarations.end(), [](const AstDeclaration& d) {
    return d.name == "begin";
  }), "anonymous begin is not a named routine");
  auto folder = std::filesystem::temp_directory_path() / "jdelphiast-qualification-include";
  std::filesystem::create_directories(folder);
  { std::ofstream include(folder / "member.inc"); include << "procedure Included;\n"; }
  Analyzer analyzer;
  analyzer.addSource(folder / "Unit.pas", "unit Unit; interface {$I member.inc} implementation end.");
  auto result = analyzer.analyze();
  require(result.units.front().ast.complete, "source-order include expansion complete");
  require(std::any_of(result.units.front().ast.declarations.begin(), result.units.front().ast.declarations.end(), [&](const AstDeclaration& d) {
    return d.name == "Included" && d.sourceFile == std::filesystem::absolute(folder / "member.inc") && d.range.begin.offset == 0 && d.range.begin.line == 1;
  }), "include candidates retain original-file provenance");
  require(std::any_of(anonymous.declarations.begin(), anonymous.declarations.end(), [](const AstDeclaration& d) {
    return d.anonymous && d.kind == "anonymousMethod" && !d.enclosingRoutine.empty();
  }), "anonymous method has an explicit node and enclosing routine");
  const std::string nestedSource = "unit N; interface implementation procedure Run; var P: reference to procedure; begin P := procedure begin if True then begin end; end; end; end.";
  auto nested = parseUnit("N.pas", nestedSource);
  auto node = std::find_if(nested.declarations.begin(), nested.declarations.end(), [](const AstDeclaration& d) { return d.anonymous; });
  require(node != nested.declarations.end(), "nested anonymous node exists");
  require(nestedSource.substr(node->range.begin.offset, node->range.end.offset - node->range.begin.offset) ==
          "procedure begin if True then begin end; end", "anonymous range includes matching end and excludes caller end");
  std::filesystem::remove_all(folder);
  // Exercise the actual concrete tokenizer/writer on supplied pilot files.
  for (int n = 1; n < argc; ++n) {
    std::ifstream input(argv[n], std::ios::binary);
    require(static_cast<bool>(input), "pilot source readable");
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    auto tree = tokenizeConcrete(bytes);
    require(tree.complete, "pilot concrete tokenization complete");
    require(writeConcrete(tree) == bytes, "pilot concrete token round trip byte identical");
    auto pilotAst = parseUnit(argv[n], bytes);
    require(!pilotAst.name.empty(), "pilot AST unit identified");
    for (const auto& declaration : pilotAst.declarations) {
      require(declaration.range.begin.offset <= declaration.range.end.offset &&
              declaration.range.end.offset <= bytes.size(), "pilot declaration range within original bytes");
    }
  }
}
