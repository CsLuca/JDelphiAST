#pragma once

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
};

struct AstDeclaration {
  std::string kind;
  std::string name;
  std::string visibility;
  std::string type;
  std::string scope;
  SourceRange range;
  std::vector<AstParameter> parameters;
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
};

struct AstAssignment {
  std::string left;
  std::string right;
  SourceRange range;
};

struct SymbolReference {
  std::string name;
  SourceRange range;
  UsesSection section{UsesSection::Implementation};
  ResolutionStatus status{ResolutionStatus::Unresolved};
  std::string declaringUnit;
};

struct UnitAst {
  std::string name;
  std::filesystem::path file;
  SourceRange range;
  std::vector<UsesItem> uses;
  std::vector<SymbolDeclaration> exports;
  std::vector<SymbolReference> references;
  std::vector<AstDeclaration> declarations;
  std::vector<AstCall> calls;
  std::vector<AstAssignment> assignments;
  std::string sourceHash;
  bool hasInitialization{};
  bool hasFinalization{};
  bool complete{true};
  bool indexOnly{};
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

struct AnalysisResult {
  std::vector<UnitAnalysis> units;
  std::vector<DependencyEdge> graph;
  std::vector<std::vector<std::string>> cycles;
};

struct IndexedUnit {
  std::string name;
  std::vector<std::string> symbols;
  bool hasInitialization{};
  bool hasFinalization{};
  bool complete{};
};

class Analyzer {
 public:
  void addSource(std::filesystem::path path, std::string source);
  void addIndexedUnit(IndexedUnit unit);
  void addUnitAlias(std::string alias, std::string declaredName);
  void setEnvironmentComplete(bool complete);
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
