#include "jdelphiast/preprocessor.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
  if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void write(const std::filesystem::path& path, const std::string& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary); output << value;
}
}

int main() {
  const auto root = std::filesystem::temp_directory_path() /
      ("jdelphiast-preprocessor-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  write(root / "defines.inc", "{$DEFINE FROM_INCLUDE}\n");
  const std::string source = R"(unit U;
interface
{$IFDEF DEBUG}
uses DebugUnit;
{$ELSE}
uses ReleaseUnit;
{$ENDIF}
{$I defines.inc}
{$IF DEFINED(FROM_INCLUDE)}
const Enabled = True;
{$ENDIF}
implementation
end.)";
  jdelphiast::PreprocessorOptions options;
  options.defines = {"RELEASE"};
  options.includePaths = {root};
  auto win32Options = options;
  win32Options.defines = {"WIN32", "CPUX86"};
  win32Options.compilerVersion = 36;
  const auto sizes = jdelphiast::preprocess(root / "Sizes.pas",
      "{$IF SizeOf(Extended) = 10} const Extended10 = 1; {$ENDIF} {$IF SizeOf(LongInt) <> SizeOf(NativeInt)} Wrong {$ELSE} Equal {$ENDIF}", win32Options);
  require(sizes.complete && sizes.source.find("Extended10") != std::string::npos && sizes.source.find("Wrong") == std::string::npos,
          "Delphi12 Win32 scalar sizeof branches use explicit target sizes");
  win32Options.defines = {"WIN64", "CPUX64"};
  require(!jdelphiast::preprocess(root / "Sizes.pas", "{$IF SizeOf(Extended) = 10} X {$ENDIF}", win32Options).complete,
          "unqualified target size remains unresolved");
  const auto result = jdelphiast::preprocess(root / "U.pas", source, options);
  require(result.source.size() == source.size(), "preprocessing preserves byte offsets");
  require(result.source.find("DebugUnit") == std::string::npos, "inactive IFDEF branch is masked");
  require(result.source.find("ReleaseUnit") != std::string::npos, "active ELSE branch remains");
  require(result.source.find("Enabled") != std::string::npos, "include DEFINE affects following source");
  require(result.includesResolved.size() == 1, "include is resolved");
  require(!result.inactiveRanges.empty(), "inactive ranges are reported");
  const auto headerAnnotations = jdelphiast::preprocess(root / "Header.pas",
      "unit Header; {$HPPEMIT '#include <x>'} {$EXTERNALSYM Value} {$NODEFINE Value} interface const Value = 1; implementation end.");
  require(headerAnnotations.complete, "C++ header annotations do not block Delphi source selection");
  const auto commentedDirective = jdelphiast::preprocess(root / "Comments.pas",
      "unit Comments; interface (* example {$ENDIF} *) {$IFDEF ABSENT} { example {$ENDIF} } {$ELSE} const Active = 1; {$ENDIF} implementation end.");
  require(commentedDirective.complete && commentedDirective.source.find("Active") != std::string::npos,
          "directive-looking text in comments does not alter conditional stack");
  const auto scoped = jdelphiast::preprocess(root / "Scoped.pas",
      "unit Scoped; interface {$SCOPEDENUMS ON} type T = (A,B); implementation end.");
  require(scoped.complete && scoped.expandedSource.find("SCOPEDENUMS ON") != std::string::npos,
          "scoped enum state is retained for semantic parsing");
  const std::string versioned = "{$IF CompilerVersion >= 36}const Modern = 1;{$ELSE}const Old = 1;{$ENDIF}";
  auto versionOptions = options;
  versionOptions.compilerVersion = 36.0;
  auto versionResult = jdelphiast::preprocess(root / "Version.pas", versioned, versionOptions);
  require(versionResult.complete && versionResult.source.find("Modern") != std::string::npos &&
          versionResult.source.find("Old") == std::string::npos, "explicit compiler version selects correct branch");
  require(!jdelphiast::preprocess(root / "Unknown.pas", versioned).complete,
          "missing compiler version remains unresolved");
  require(jdelphiast::preprocess(root / "Switch.pas", "{$B-,R+,I+,Q+}const A=1;").complete,
          "combined checking switches preserve declarations");
  auto rtti = jdelphiast::preprocess(root / "RTTI.pas", "{$M+}type T=class end;");
  require(rtti.complete && rtti.expandedSource.find("{$M+}") != std::string::npos,
          "semantic RTTI switch retained for AST state");
  require(jdelphiast::preprocess(root / "Linker.pas", "{$MINSTACKSIZE $4000}{$MAXSTACKSIZE $100000}{$IMAGEBASE $400000}unit L; interface implementation end.").complete,
          "linker layout directives do not block source resolution");
  auto compound = jdelphiast::preprocess(root / "Compound.pas",
      "{$IF (CompilerVersion >= 36) AND NOT DEFINED(ABSENT) OR DEFINED(OTHER)}const Selected=1;{$ELSE}const Wrong=1;{$ENDIF}", versionOptions);
  require(compound.complete && compound.source.find("Selected") != std::string::npos && compound.source.find("Wrong") == std::string::npos,
          "compound condition respects comparisons, parentheses and precedence");
  auto precedence = jdelphiast::preprocess(root / "Precedence.pas",
      "{$IF DEFINED(ABSENT) AND DEFINED(OTHER) OR NOT DEFINED(ABSENT)}const Selected=1;{$ENDIF}", versionOptions);
  require(precedence.complete && precedence.source.find("Selected") != std::string::npos, "AND binds tighter than OR");
  require(!jdelphiast::preprocess(root / "Unsupported.pas",
      "{$IF DEFINED(ABSENT) AND SizeOf(Integer)=4}const Unsafe=1;{$ENDIF}", versionOptions).complete,
          "unsupported operand is not hidden by false left operand");
  require(!jdelphiast::preprocess(root / "Malformed.pas",
      "{$IF DEFINED(A B)}const Unsafe=1;{$ENDIF}", versionOptions).complete,
          "invalid DEFINED argument rejected");
  std::filesystem::remove_all(root);
  std::cout << "All preprocessor tests passed\n";
}
