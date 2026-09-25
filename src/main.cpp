#include "jdelphiast/analyzer.hpp"
#include "jdelphiast/project.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>

int main(int argc, char** argv) {
  bool json = false;
  int fileCount = 0;
  int sourceFileCount = 0;
  jdelphiast::Analyzer analyzer;
  jdelphiast::ProjectOptions projectOptions;
  std::filesystem::path package;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--json") {
      json = true;
      continue;
    }
    if ((argument == "--index" || argument == "--search-path") && i + 1 >= argc) {
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
  if (fileCount == 0) {
    std::cerr << "Usage: jdelphiast_cli [--json] [--index file.jdi] [--search-path dir] "
                 "package.dpk | unit1.pas [unit2.pas ...]\n";
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
      std::cout << (json ? jdelphiast::toJson(result) : jdelphiast::toText(result));
    } else {
      for (const auto& index : projectOptions.indexFiles)
        for (auto unit : jdelphiast::loadSymbolIndex(index)) analyzer.addIndexedUnit(std::move(unit));
      const auto result = analyzer.analyze();
      std::cout << (json ? jdelphiast::toJson(result) : jdelphiast::toText(result));
    }
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
