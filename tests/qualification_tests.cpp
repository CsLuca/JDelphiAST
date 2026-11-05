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
  auto vendorShape = parseUnit("Vendor.pas", "unit Vendor; interface type IJson = interface; IJson = interface procedure Clear; end; TBase<T> = class end; TDerived<T> = class(TBase<T>); function SO(JSON: string = '{}'): IJson; overload; function SO(const Args: array of const): IJson; overload; implementation end.");
  require(std::count_if(vendorShape.declarations.begin(), vendorShape.declarations.end(), [](const auto& d) { return d.name == "IJson" && d.kind == "interface"; }) == 1,
          "forward interface consolidated with full definition");
  require(std::count_if(vendorShape.declarations.begin(), vendorShape.declarations.end(), [](const auto& d) { return d.name == "SO" && d.ownerType.empty(); }) == 2,
          "global overloads after ancestor-only class preserved");
  require(std::none_of(vendorShape.declarations.begin(), vendorShape.declarations.end(), [](const auto& d) { return d.name == "TDerived.SO"; }),
          "ancestor-only class does not own following global functions");
  require(std::any_of(vendorShape.declarations.begin(), vendorShape.declarations.end(), [](const auto& d) {
    return d.name == "SO" && d.parameters.size() == 1 && d.parameters[0].type == "array of const";
  }), "open array of const parameter retains complete type");
  require(std::any_of(vendorShape.declarations.begin(), vendorShape.declarations.end(), [](const auto& d) {
    return d.name == "SO" && d.parameters.size() == 1 && d.parameters[0].defaultValue == "'{}'";
  }), "SO string default retained verbatim");
  auto typelibSwitches = parseUnit("TypeLib.pas", "unit TypeLib; interface {$TYPEDADDRESS OFF}{$WRITEABLECONST ON}{$VARPROPSETTER ON} type TImported = interface procedure SetValue(const V: Variant); end; implementation end.");
  require(typelibSwitches.complete, "named type-library switches parsed");
  const auto imported = std::find_if(typelibSwitches.declarations.begin(), typelibSwitches.declarations.end(), [](const auto& d) { return d.name == "TImported.SetValue"; });
  require(imported != typelibSwitches.declarations.end() && imported->typedAddressMode == "UNTYPED" &&
          imported->typedConstantMode == "WRITABLE" && imported->variantPropertySetterMode == "ENABLED",
          "type-library switch modes retained on members");
  auto switchAst = parseUnit("Switches.pas", "unit Switches; interface {$H-,M+,J+} type T = class end; function ShortValue(const X: string): string; {$H+} function LongValue: string; implementation end.");
  auto shortValue = std::find_if(switchAst.declarations.begin(), switchAst.declarations.end(), [](const auto& d) { return d.name == "ShortValue"; });
  require(shortValue != switchAst.declarations.end() && shortValue->type == "ShortString" && shortValue->parameters[0].type == "ShortString",
          "H minus normalizes actual string parameter and return types");
  require(shortValue->rttiMode == "ENABLED" && shortValue->typedConstantMode == "WRITABLE",
          "M and J semantic state recorded rather than ignored");
  auto longValue = std::find_if(switchAst.declarations.begin(), switchAst.declarations.end(), [](const auto& d) { return d.name == "LongValue"; });
  require(longValue != switchAst.declarations.end() && longValue->type == "string" && longValue->stringMode == "LONGSTRING",
          "H plus transition restores long string mode");
  auto sdkShape = parseUnit("Sdk.pas", "unit Sdk; interface type TQueryClass = class of TQuery; "
    "TField = class function Value: string; end; "
    "TQuery = class(TObject) public procedure SetText(const AValue: string); "
    "class function GetNew(const ASql: string): TQuery; end; "
    "function GetField(ADB: TObject; const ATable, AField: string; const AWhere: string): Variant; "
    "implementation end.");
  require(std::any_of(sdkShape.exports.begin(), sdkShape.exports.end(), [](const SymbolDeclaration& s) {
    return s.name == "TQuery" && s.kind == "class";
  }), "class after const parameter remains exported");
  require(std::none_of(sdkShape.exports.begin(), sdkShape.exports.end(), [](const SymbolDeclaration& s) {
    return s.name == "AValue" || s.name == "ATable" || s.name == "AWhere";
  }), "const parameters are not unit exports");
  require(std::any_of(sdkShape.declarations.begin(), sdkShape.declarations.end(), [](const AstDeclaration& d) {
    return d.name == "GetField" && d.ownerType.empty() && d.parameters.size() == 4;
  }), "global function after class function retains unit ownership and parameters");
  require(std::none_of(sdkShape.declarations.begin(), sdkShape.declarations.end(), [](const AstDeclaration& d) {
    return d.name == "TQuery.GetField" || d.name == "TQueryClass.Value";
  }), "metaclass and class method do not consume later declarations");
  auto invalid = parseUnit("Bad.pas", "unit Bad; interface procedure Run(; implementation end.");
  auto indexedProperty = parseUnit("Items.pas", "unit Items; interface type TList = class public property Items[Index: NativeInt]: Pointer read Get; end; implementation end.");
  require(std::any_of(indexedProperty.declarations.begin(), indexedProperty.declarations.end(), [](const AstDeclaration& d) {
    return d.name == "TList.Items" && d.type == "Pointer";
  }), "indexed property retains declared value type");
  auto indexed = parseUnit("Indexed.pas", "unit Indexed; interface implementation procedure Run; begin CampiObj['IdArtico'] := Query.FieldByName('IdLotSer').AsInteger; end; end.");
  require(std::any_of(indexed.assignments.begin(), indexed.assignments.end(), [](const AstAssignment& assignment) {
    return assignment.left == "CampiObj['IdArtico']" && assignment.right == "Query.FieldByName('IdLotSer').AsInteger";
  }), "indexed ID producer/consumer assignment retained without inferred domain");
  require(!invalid.complete && invalid.name == "Bad", "malformed source returns incomplete without exception");
  Analyzer shadowProbe;
  IndexedUnit foreignLista;
  foreignLista.name = "ForeignLista";
  foreignLista.symbols = {"Lista"};
  foreignLista.complete = true;
  shadowProbe.addIndexedUnit(foreignLista);
  shadowProbe.addSource("LocalLista.pas", "unit LocalLista; interface uses ForeignLista; implementation procedure Run; var Lista: TObject; begin Lista.Free; end; end.");
  const auto shadowResult = shadowProbe.analyze();
  require(std::none_of(shadowResult.units.front().dependencies.front().references.begin(), shadowResult.units.front().dependencies.front().references.end(), [](const auto& r) {
    return r.name == "Lista.Free";
  }), "local receiver is not attributed to foreign same-named export");
  Analyzer memberProbe;
  memberProbe.addIndexedUnit(foreignLista);
  memberProbe.addSource("MemberLista.pas", "unit MemberLista; interface uses ForeignLista; type TView = class private FLista: TObject; public property Lista: TObject read FLista; procedure Run; end; implementation procedure TView.Run; begin Lista.Free; end; end.");
  const auto memberResult = memberProbe.analyze();
  require(std::none_of(memberResult.units.front().dependencies.front().references.begin(), memberResult.units.front().dependencies.front().references.end(), [](const auto& r) { return r.name == "Lista.Free"; }),
          "own property receiver is not attributed to foreign same-named export");
  IndexedUnit baseLista;
  baseLista.name = "BaseLista"; baseLista.complete = true; baseLista.symbols = {"TBaseView"};
  AstDeclaration listaProperty;
  listaProperty.name = "TBaseView.Lista"; listaProperty.ownerType = "TBaseView";
  listaProperty.kind = "property"; listaProperty.type = "TObject";
  baseLista.declarations.push_back(listaProperty);
  Analyzer inheritedProbe;
  inheritedProbe.addIndexedUnit(foreignLista); inheritedProbe.addIndexedUnit(baseLista);
  inheritedProbe.addSource("InheritedLista.pas", "unit InheritedLista; interface uses ForeignLista, BaseLista; type TView = class(TBaseView) procedure Run; end; implementation procedure TView.Run; begin Lista.Free; end; end.");
  const auto inheritedResult = inheritedProbe.analyze();
  require(std::none_of(inheritedResult.units.front().dependencies.front().references.begin(), inheritedResult.units.front().dependencies.front().references.end(), [](const auto& r) { return r.name == "Lista.Free"; }),
          "inherited property receiver does not bind unrelated unit export");
  require(std::none_of(memberResult.units.front().ast.references.begin(), memberResult.units.front().ast.references.end(), [](const auto& r) { return r.name == "Run"; }),
           "qualified implementation declaration suffix is not an invocation reference");
  Analyzer typedProbe;
  typedProbe.addIndexedUnit(foreignLista);
  typedProbe.addSource("Lists.pas", "unit Lists; interface type TList = class public property Count: Integer read GetCount; procedure Clear; end; TBase = class public property Lista: TList read GetLista; end; implementation end.");
  typedProbe.addSource("Typed.pas", "unit Typed; interface uses Lists, ForeignLista; type TView = class(TBase) procedure Run; end; implementation procedure TView.Run; var Local: TList; begin Lista.Count; Local.Clear; end; end.");
  const auto typedResult = typedProbe.analyze();
  const auto& typedRefs = typedResult.units[1].ast.references;
  require(std::any_of(typedRefs.begin(), typedRefs.end(), [](const auto& r) {
    return r.name == "Lista.Count" && r.status == ResolutionStatus::Resolved && r.declaringUnit == "Lists" &&
           r.resolvedDeclaration == "TList.Count" && r.receiverType == "TList" && r.receiverDeclaration == "Lists.TBase.Lista";
  }), "inherited property receiver resolves to its declared type member");
  require(std::any_of(typedRefs.begin(), typedRefs.end(), [](const auto& r) {
    return r.name == "Local.Clear" && r.status == ResolutionStatus::Resolved && r.resolvedDeclaration == "TList.Clear";
  }), "local receiver retained and resolves within its routine");
  Analyzer ambiguousTypeProbe;
  ambiguousTypeProbe.addSource("Left.pas", "unit Left; interface type TList = class procedure Clear; end; implementation end.");
  ambiguousTypeProbe.addSource("Right.pas", "unit Right; interface type TList = class procedure Clear; end; implementation end.");
  ambiguousTypeProbe.addSource("UseBoth.pas", "unit UseBoth; interface uses Left, Right; implementation procedure Run; var Value: TList; begin Value.Clear; end; end.");
  const auto ambiguousTyped = ambiguousTypeProbe.analyze();
  require(std::any_of(ambiguousTyped.units[2].ast.references.begin(), ambiguousTyped.units[2].ast.references.end(), [](const auto& r) {
    return r.name == "Value.Clear" && r.status == ResolutionStatus::Unresolved;
  }), "ambiguous imported type cannot authorize member ownership");
  Analyzer indexedInheritanceProbe;
  IndexedUnit indexedLists;
  indexedLists.name = "IndexedLists"; indexedLists.complete = true;
  indexedLists.declarations = parseUnit("IndexedLists.pas", "unit IndexedLists; interface type TList = class public property Count: Integer read GetCount; end; implementation end.").declarations;
  IndexedUnit indexedBase;
  indexedBase.name = "IndexedBase"; indexedBase.complete = true; indexedBase.dependencies = {"IndexedLists"};
  auto indexedBaseAst = parseUnit("IndexedBase.pas", "unit IndexedBase; interface uses IndexedLists; type TBase = class public property Lista: TList read GetLista; end; TMiddle = class(TBase); implementation end.");
  indexedBase.declarations = indexedBaseAst.declarations; indexedBase.inheritance = indexedBaseAst.inheritance;
  indexedInheritanceProbe.addIndexedUnit(indexedLists); indexedInheritanceProbe.addIndexedUnit(indexedBase);
  indexedInheritanceProbe.addSource("IndexedView.pas", "unit IndexedView; interface uses IndexedBase; type TView = class(TMiddle) procedure Run; end; implementation procedure TView.Run; begin Lista.Count; end; end.");
  const auto indexedInheritance = indexedInheritanceProbe.analyze();
  require(std::any_of(indexedInheritance.units.front().ast.references.begin(), indexedInheritance.units.front().ast.references.end(), [](const auto& r) {
    return r.name == "Lista.Count" && r.status == ResolutionStatus::Resolved && r.declaringUnit == "IndexedLists" && r.resolvedDeclaration == "TList.Count";
  }), "indexed transitive ancestry uses declaring unit dependencies for property type");
  Analyzer constructorProbe;
  constructorProbe.addSource("Forms.pas", "unit Forms; interface type TCustomForm = class public constructor Create(Owner: TObject); end; TForm = class(TCustomForm); implementation end.");
  constructorProbe.addSource("LocalForm.pas", "unit LocalForm; interface uses Forms; type TLocalForm = class(TForm); implementation procedure Run; var Form: TLocalForm; procedure Helper; begin with Form do begin end; end; begin Form := TLocalForm.Create(nil); with Form do begin end; end; end.");
  const auto constructorResult = constructorProbe.analyze();
  require(std::any_of(constructorResult.units[1].ast.references.begin(), constructorResult.units[1].ast.references.end(), [](const auto& r) {
    return r.name == "TLocalForm.Create" && r.status == ResolutionStatus::Resolved && r.resolvedDeclaration == "TCustomForm.Create";
  }), "explicit local class constructor resolves ancestor despite later with block");
  Analyzer signatureProbe;
  signatureProbe.addSource("Api.pas", "unit Api; interface type TApi = class public procedure Run(Value: Integer); procedure Optional(Value: Integer = 1); procedure Change(var Value: Integer); end; implementation end.");
  signatureProbe.addSource("Calls.pas", "unit Calls; interface uses Api; implementation procedure Test(Api: TApi; Value: Integer); begin Api.Run(Value); Api.Optional(); Api.Change(Value); Api.Run(Unknown); Api.Run(Value, Value); end; end.");
  const auto signatureResult = signatureProbe.analyze();
  const auto& signatureCalls = signatureResult.units[1].ast.calls;
  require(std::count_if(signatureCalls.begin(), signatureCalls.end(), [](const AstCall& c) { return c.signatureSelected; }) == 3,
          "exact scoped parameter types, optional arity and var lvalue select declarations");
  require(std::any_of(signatureCalls.begin(), signatureCalls.end(), [](const AstCall& c) {
    return c.name == "Api.Run" && c.signatureStatus == "ARGUMENT_COMPATIBILITY_REVIEW" && !c.signatureSelected;
  }), "unknown argument never selects signature");
  require(std::any_of(signatureCalls.begin(), signatureCalls.end(), [](const AstCall& c) {
    return c.name == "Api.Run" && c.signatureStatus == "ARITY_MISMATCH_OR_NONCALLABLE" && !c.signatureSelected;
  }), "wrong arity remains explicit");
  Analyzer signatureNegativeProbe;
  signatureNegativeProbe.addSource("NegativeApi.pas", "unit NegativeApi; interface type TApi = class public procedure Change(var Value: Integer); procedure Pick(Value: Integer); overload; procedure Pick(Value: Int64); overload; end; implementation end.");
  signatureNegativeProbe.addSource("NegativeCalls.pas", "unit NegativeCalls; interface uses NegativeApi; implementation procedure Test(Api: TApi; const Value: Integer); begin Api.Change(Value); Api.Pick(Value); Api.Pick(1); end; end.");
  const auto negativeSignatures = signatureNegativeProbe.analyze();
  require(std::none_of(negativeSignatures.units[1].ast.calls.begin(), negativeSignatures.units[1].ast.calls.end(), [](const AstCall& c) { return c.signatureSelected; }),
          "const var argument, uncertain convertible overload and literal do not select signatures");
  Analyzer derivedSignatureProbe;
  derivedSignatureProbe.addSource("Hierarchy.pas", "unit Hierarchy; interface type TBase = class end; TChild = class(TBase); TApi = class public procedure Accept(Value: TBase); procedure Mutate(var Value: TBase); end; implementation end.");
  derivedSignatureProbe.addSource("DerivedCalls.pas", "unit DerivedCalls; interface uses Hierarchy; implementation procedure Run(Api: TApi; Child: TChild; Base: TBase); begin Api.Accept(Child); Api.Mutate(Child); Api.Accept(Base); end; end.");
  const auto derivedSignatures = derivedSignatureProbe.analyze();
  require(std::any_of(derivedSignatures.units[1].ast.calls.begin(), derivedSignatures.units[1].ast.calls.end(), [](const AstCall& c) {
    return c.name == "Api.Accept" && c.signatureSelected && c.signatureStatus == "DECLARED_CLASS_ASSIGNABLE";
  }), "derived class argument is assignable by verified ancestry");
  require(std::any_of(derivedSignatures.units[1].ast.calls.begin(), derivedSignatures.units[1].ast.calls.end(), [](const AstCall& c) {
    return c.name == "Api.Mutate" && !c.signatureSelected;
  }), "var parameter does not accept derived type substitution");
  Analyzer missingBaseProbe;
  missingBaseProbe.addSource("BrokenHierarchy.pas", "unit BrokenHierarchy; interface type TBase = class end; TChild = class(TMissing); TApi = class public procedure Accept(Value: TBase); end; implementation end.");
  missingBaseProbe.addSource("BrokenCalls.pas", "unit BrokenCalls; interface uses BrokenHierarchy; implementation procedure Run(Api: TApi; Child: TChild); begin Api.Accept(Child); end; end.");
  const auto missingBase = missingBaseProbe.analyze();
  require(std::none_of(missingBase.units[1].ast.calls.begin(), missingBase.units[1].ast.calls.end(), [](const AstCall& c) { return c.signatureSelected; }),
          "missing ancestor cannot demonstrate class assignability");
  Analyzer systemConstructorProbe;
  systemConstructorProbe.addSource("System.pas", "unit System; interface type TObject = class public constructor Create; end; implementation end.");
  systemConstructorProbe.addSource("Lists.pas", "unit Lists; interface type TList = class(TObject); implementation end.");
  systemConstructorProbe.addSource("UseList.pas", "unit UseList; interface uses Lists; implementation procedure Run; begin TList.Create(); end; end.");
  const auto systemConstructor = systemConstructorProbe.analyze();
  require(std::any_of(systemConstructor.units[2].ast.calls.begin(), systemConstructor.units[2].ast.calls.end(), [](const AstCall& c) {
    return c.qualifiedMember == "System.TObject.Create" && c.signatureSelected;
  }), "implicit System visibility resolves actual TObject constructor ancestor");
  auto proceduralTypes = parseUnit("Events.pas", "unit Events; interface type TEvent = procedure (Sender: TObject; var Action: Integer) of object; TFunc = function(X: Integer): string; implementation end.");
  require(proceduralTypes.complete, "procedural type signatures do not require anonymous bodies");
  const std::string multilineSource = "unit Multiline; interface const SQL = '''\r\nSELECT (ID) FROM T WHERE NAME='value'\r\n'''; type TModel = class end; implementation end.";
  const auto multilineConcrete = tokenizeConcrete(multilineSource);
  require(multilineConcrete.complete && writeConcrete(multilineConcrete) == multilineSource,
          "Delphi multiline string is one lossless literal envelope");
  require(parseUnit("Multiline.pas", multilineSource).complete,
          "SQL inside multiline literal does not become executable syntax");
  const std::string escapedQuotes = "unit Quotes; interface implementation procedure P; begin S := ''''''\n; end; end.";
  require(tokenizeConcrete(escapedQuotes).complete && parseUnit("Quotes.pas", escapedQuotes).complete,
          "even run of quote escapes is not mistaken for multiline opening");
  require(std::none_of(proceduralTypes.declarations.begin(), proceduralTypes.declarations.end(), [](const auto& d) { return d.anonymous; }),
          "procedural types do not become anonymous method nodes");
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
  auto indexFolder = std::filesystem::temp_directory_path() / "jdelphiast-configured-index";
  std::filesystem::create_directories(indexFolder);
  { std::ofstream f(indexFolder / "Sdk.pas"); f << "unit Sdk; interface {$IFDEF PICKED} type TPicked = class end; {$ELSE} type TWrong = class end; {$ENDIF} {$I extra.inc} implementation end."; }
  { std::ofstream f(indexFolder / "extra.inc"); f << "type TIncluded = class end;"; }
  { std::ofstream f(indexFolder / "Sdk.dproj"); f << "<Project><PropertyGroup><DCC_Define>PICKED</DCC_Define></PropertyGroup></Project>"; }
  IndexBuildOptions configuredIndex;
  configuredIndex.version = "v600";
  configuredIndex.projectOptions.dprojFile = indexFolder / "Sdk.dproj";
  { std::ofstream f(indexFolder / "SwitchState.pas"); f << "unit SwitchState; interface {$H-,M+,J+} function Value(const X: string): string; implementation end."; }
  const auto stateIndex = buildSymbolIndex({indexFolder / "SwitchState.pas"}, configuredIndex);
  require(!stateIndex.hasBlockingErrors, "state roundtrip index builds");
  { std::ofstream f(indexFolder / "state.jdi"); f << stateIndex.content; }
  const auto loadedStates = loadSymbolIndex(indexFolder / "state.jdi");
  { std::ofstream f(indexFolder / "Defaults.pas"); f << "unit Defaults; interface function Value(Text: string = 'a;b|c'): string; implementation end."; }
  auto defaultsIndex = buildSymbolIndex({indexFolder / "Defaults.pas"}, configuredIndex);
  { std::ofstream f(indexFolder / "defaults.jdi"); f << defaultsIndex.content; }
  auto loadedDefaults = loadSymbolIndex(indexFolder / "defaults.jdi");
  require(loadedDefaults.front().declarations.front().parameters[0].defaultValue == "'a;b|c'",
          "default delimiters survive JDI encoding without corrupting signature parameters");
  const auto loadedValue = std::find_if(loadedStates.front().declarations.begin(), loadedStates.front().declarations.end(), [](const auto& d) { return d.name == "Value"; });
  require(loadedValue != loadedStates.front().declarations.end() && loadedValue->stringMode == "SHORTSTRING" &&
          loadedValue->rttiMode == "ENABLED" && loadedValue->typedConstantMode == "WRITABLE",
          "H M J states survive JDI load");
  const auto cachedStates = loadSymbolIndexCached(indexFolder / "state.jdi");
  const auto cachedValue = std::find_if(cachedStates.front().declarations.begin(), cachedStates.front().declarations.end(), [](const auto& d) { return d.name == "Value"; });
  require(cachedValue != cachedStates.front().declarations.end() && cachedValue->parameters[0].type == "ShortString" &&
          cachedValue->typedConstantMode == "WRITABLE", "cached index preserves state and short string signature");
  { std::ofstream f(indexFolder / "Enums.pas"); f << "unit Enums; interface {$SCOPEDENUMS ON} type TScoped = (ScopedOnly); {$SCOPEDENUMS OFF} type TGlobal = (GlobalValue); implementation end."; }
  auto enumIndex = buildSymbolIndex({indexFolder / "Enums.pas"}, configuredIndex);
  require(!enumIndex.hasBlockingErrors, "scoped enum index complete");
  auto enumAst = parseUnit(indexFolder / "Enums.pas", "unit Enums; interface {$SCOPEDENUMS ON} type TScoped = (ScopedOnly); {$SCOPEDENUMS OFF} type TGlobal = (GlobalValue); implementation end.");
  require(std::none_of(enumAst.exports.begin(), enumAst.exports.end(), [](const auto& s) { return s.name == "ScopedOnly"; }),
          "scoped value not exported globally");
  require(std::any_of(enumAst.exports.begin(), enumAst.exports.end(), [](const auto& s) { return s.name == "GlobalValue"; }),
          "OFF restores global enum value export");
  require(std::any_of(enumAst.declarations.begin(), enumAst.declarations.end(), [](const auto& d) { return d.name == "TScoped.ScopedOnly"; }),
          "scoped enum qualified value retained");
  { std::ofstream f(indexFolder / "enum-state.inc"); f << "{$SCOPEDENUMS ON}"; }
  { std::ofstream f(indexFolder / "IncludedEnums.pas"); f << "unit IncludedEnums; interface {$I enum-state.inc} type TIncludedScoped = (IncludedOnly); implementation end."; }
  auto includedEnumIndex = buildSymbolIndex({indexFolder / "IncludedEnums.pas"}, configuredIndex);
  require(!includedEnumIndex.hasBlockingErrors && includedEnumIndex.content.find("TIncludedScoped.IncludedOnly") != std::string::npos,
          "scoped enum state follows include expansion order");
  require(includedEnumIndex.content.find("|IncludedOnly,") == std::string::npos,
          "included scoped value not globally exported");
  auto indexResult = buildSymbolIndex({indexFolder / "Sdk.pas"}, configuredIndex);
  require(!indexResult.hasBlockingErrors, "configured index preprocessing succeeds");
  require(indexResult.content.find("TPicked") != std::string::npos && indexResult.content.find("TWrong") == std::string::npos,
          "DPROJ defines select indexed active branch only");
  require(indexResult.content.find("TIncluded") != std::string::npos, "configured index expands include exports");
  require(indexResult.content.find("configuration_sha256=") != std::string::npos, "configuration identity recorded");
  require(indexResult.sourceFingerprints.size() == 1 && indexResult.includeFingerprints.size() == 1,
          "source and include hashes recorded for configured coverage");
  { std::ofstream f(indexFolder / "Sdk.dpk"); f << "package Sdk; contains SdkUnit in 'Sdk.pas'; end."; }
  auto packageIndex = buildSymbolIndex({indexFolder / "Sdk.dpk"}, configuredIndex);
  require(packageIndex.statistics.unitsIndexed == 1 && packageIndex.content.find("package=Sdk") != std::string::npos,
          "explicit package indexes members with real package metadata");
  std::filesystem::create_directories(indexFolder / "rtl");
  { std::ofstream f(indexFolder / "rtl" / "External.pas"); f << "unit External; interface type TExternal = class end; implementation end."; }
  { std::ofstream f(indexFolder / "ExternalPackage.dpk"); f << "package ExternalPackage; contains External; end."; }
  auto explicitSearch = configuredIndex;
  explicitSearch.projectOptions.searchPaths.push_back(indexFolder / "rtl");
  require(buildSymbolIndex({indexFolder / "ExternalPackage.dpk"}, explicitSearch).statistics.unitsIndexed == 1,
          "bare package member resolves from explicit authoritative search path");
  std::filesystem::remove(indexFolder / "extra.inc");
  require(buildSymbolIndex({indexFolder / "Sdk.pas"}, configuredIndex).hasBlockingErrors,
          "missing include blocks configured index authority");
  std::filesystem::remove_all(indexFolder);
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
