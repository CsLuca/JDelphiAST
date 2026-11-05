#pragma once
#include <optional>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jdelphiast {

struct SourcePosition {
  std::size_t offset{};
  std::size_t line{1};
  std::size_t column{1};
};

struct SourceRange {
  SourcePosition begin;
  SourcePosition end;
  std::filesystem::path sourceFile;
};

// Lossless source-oriented token envelope. Ranges and columns count bytes,
// not Unicode code points. Trivia and BOM are retained verbatim.
struct ConcreteToken {
  std::string kind;
  std::string bytes;
  SourceRange range;
};
struct ConcreteSyntax {
  std::vector<ConcreteToken> tokens;
  bool complete{true};
  std::vector<std::string> diagnostics;
};
[[nodiscard]] ConcreteSyntax tokenizeConcrete(std::string_view source);
[[nodiscard]] std::string writeConcrete(const ConcreteSyntax& syntax);
// Valid UTF8 stays intact; invalid UTF8 bytes are represented losslessly as
// U+00xx in JSON. This is a byte envelope, not an inferred ANSI code page.
[[nodiscard]] std::string jsonSourceBytes(std::string_view source);

struct InactiveRange {
  std::filesystem::path file;
  SourceRange range;
  std::string condition;
};

enum class UsesSection { Interface, Implementation };
enum class DependencyStatus { Used, Unused, SideEffect, Unknown };
enum class Confidence { High, Medium, Low };
enum class ResolutionStatus { Resolved, Ambiguous, Unresolved, Builtin };

struct UsesItem {
  std::string name;
  UsesSection section{UsesSection::Interface};
  SourceRange range;
};

struct SymbolDeclaration {
  std::string name;
  std::string kind;
  SourceRange range;
};

struct AstParameter {
  std::string name;
  std::string type;
  SourceRange range;
  std::string modifier;
  std::optional<std::string> defaultValue;
};

struct AstDeclaration {
  std::string kind;
  std::string name;
  std::string visibility;
  std::string type;
  std::string scope;
  SourceRange range;
  std::vector<AstParameter> parameters;
  std::string signature;
  std::string ownerType;
  bool overload{};
  bool isOverride{};
  bool reintroduced{};
  bool deprecated{};
  std::filesystem::path sourceFile;
  bool anonymous{};
  std::string enclosingRoutine;
  SourceRange bodyRange;
  std::string stringMode;
  std::string rttiMode;
  std::string typedConstantMode;
  std::string typedAddressMode;
  std::string variantPropertySetterMode;
  bool forwardDeclaration{};
};

struct InheritanceRelation {
  std::string type;
  std::string kind;
  std::string baseType;
  SourceRange range;
};

struct CallArgument {
  std::string kind;
  std::string text;
  std::string resolvedType;
  SourceRange range;
};

struct AssignmentTarget {
  std::string kind;
  std::string text;
  std::string resolvedType;
};

struct AstCall {
  std::string name;
  SourceRange range;
  std::vector<CallArgument> arguments;
  AssignmentTarget assignmentTarget;
  std::string resolvedReturnType;
  std::string qualifiedMember;
  std::string signatureStatus;
  std::string selectedSignature;
  bool signatureSelected{};
  std::vector<std::string> candidateSignatures;
};

struct AstAssignment {
  std::string left;
  std::string right;
  std::string targetType;
  std::string valueKind;
  std::string resolvedReturnType;
  std::vector<CallArgument> arguments;
  SourceRange range;
};

struct SymbolReference {
  std::string name;
  SourceRange range;
  UsesSection section{UsesSection::Implementation};
  ResolutionStatus status{ResolutionStatus::Unresolved};
  std::string declaringUnit;
  std::vector<std::string> candidates;
  std::string resolvedDeclaration;
  std::string receiverType;
  std::string memberType;
  std::string resolutionMethod;
  std::string resolvedKind;
  std::string receiverDeclaration;
};

struct UnitAst {
  std::vector<ConcreteToken> concreteTokens;
  struct ParseDiagnostic {
    std::string code;
    std::string message;
    SourcePosition position;
  };
  std::string name;
  std::filesystem::path file;
  SourceRange range;
  std::vector<UsesItem> uses;
  std::vector<SymbolDeclaration> exports;
  std::vector<SymbolReference> references;
  std::vector<AstDeclaration> declarations;
  std::vector<AstCall> calls;
  std::vector<AstAssignment> assignments;
  std::vector<ParseDiagnostic> parseDiagnostics;
  std::string sourceHash;
  std::vector<std::filesystem::path> includesResolved;
  std::vector<InactiveRange> inactiveRanges;
  std::vector<std::string> preprocessorReasons;
  std::vector<std::string> sideEffectReasons;
  bool hasInitialization{};
  bool hasFinalization{};
  bool complete{true};
  bool indexOnly{};
  std::vector<InheritanceRelation> inheritance;
  std::string sourceIndex{"project"};
};

struct Dependency {
  std::string unit;
  UsesSection section{UsesSection::Interface};
  DependencyStatus status{DependencyStatus::Unknown};
  Confidence confidence{Confidence::Low};
  std::vector<SymbolReference> references;
  std::vector<std::string> reasons;
};

struct UnitAnalysis {
  UnitAst ast;
  std::vector<Dependency> dependencies;
};

struct DependencyEdge {
  std::string fromUnit;
  std::string toUnit;
  UsesSection section{UsesSection::Interface};
  DependencyStatus status{DependencyStatus::Unknown};
};

struct UnitMappingSuggestion {
  std::string sourceUnit;
  std::string targetUnit;
  std::string mappingType;
  std::string confidence;
  std::string compatibility;
  std::string automaticAction;
  std::string notes;
};

struct AnalysisResult {
  std::vector<UnitAnalysis> units;
  std::vector<DependencyEdge> graph;
  std::vector<std::vector<std::string>> cycles;
  std::string configuration;
  std::string platform;
  std::vector<std::string> defines;
  std::vector<std::filesystem::path> includesResolved;
  std::vector<InactiveRange> inactiveRanges;
  bool preprocessorComplete{true};
  std::vector<std::string> diagnostics;
  std::filesystem::path unitMappingCatalog;
  bool unitMappingCatalogValid{};
  std::vector<UnitMappingSuggestion> unitMappings;
};

struct IndexedUnit {
  struct SourceEvidence {
    std::string symbol;
    std::size_t line{};
    std::size_t column{};
    std::string usage;
  };
  std::string name;
  std::vector<std::string> symbols;
  std::vector<AstDeclaration> declarations;
  bool hasInitialization{};
  bool hasFinalization{};
  bool complete{};
  std::vector<InheritanceRelation> inheritance;
  std::filesystem::path sourceFile;
  std::string indexVersion{"unknown"};
  std::string packageName;
  std::string packageDcp;
  std::string packageBpl;
  std::filesystem::path sourceProject;
  std::vector<std::string> dependencies;
  std::vector<SourceEvidence> sourceEvidence;
  std::string origin;
};

class Analyzer {
 public:
  void addSource(std::filesystem::path path, std::string source);
  void addIndexedUnit(IndexedUnit unit);
  void addUnitAlias(std::string alias, std::string declaredName);
  void setEnvironmentComplete(bool complete);
  void setPreprocessor(std::vector<std::string> defines,
                        std::vector<std::filesystem::path> includePaths,
                        std::optional<double> compilerVersion = std::nullopt);
  void setBuildContext(std::string configuration, std::string platform);
  void addDiagnostic(std::string diagnostic);
  void setUnitMappingCatalog(std::filesystem::path catalog, bool valid,
                             std::vector<UnitMappingSuggestion> mappings = {});
  [[nodiscard]] AnalysisResult analyze() const;

 private:
  struct Input {
    std::filesystem::path path;
    std::string source;
  };
  std::vector<Input> inputs_;
  std::vector<IndexedUnit> indexedUnits_;
  std::unordered_map<std::string, std::string> unitAliases_;
  bool environmentComplete_{true};
  std::vector<std::string> defines_;
  std::vector<std::filesystem::path> includePaths_;
  std::optional<double> compilerVersion_;
  std::string configuration_;
  std::string platform_;
  std::vector<std::string> diagnostics_;
  std::filesystem::path unitMappingCatalog_;
  bool unitMappingCatalogValid_{};
  std::vector<UnitMappingSuggestion> unitMappings_;
};

[[nodiscard]] UnitAst parseUnit(std::filesystem::path path, const std::string& source);

[[nodiscard]] std::string toText(const AnalysisResult& result);
[[nodiscard]] std::string toJson(const AnalysisResult& result);
[[nodiscard]] std::string toProjectJson(const AnalysisResult& result, std::string_view plugin,
                                        const std::filesystem::path& sourceRoot,
                                        std::string_view generatedAt);
[[nodiscard]] const char* toString(DependencyStatus value);
[[nodiscard]] const char* toString(Confidence value);

}  // namespace jdelphiast
