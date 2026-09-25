#include "jdelphiast/analyzer.hpp"
#include "jdelphiast/project.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>

namespace {

std::string timestamp() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
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

}  // namespace

int main(int argc, char** argv) {
  bool json = false;
  int fileCount = 0;
  int sourceFileCount = 0;
  jdelphiast::Analyzer analyzer;
  jdelphiast::ProjectOptions projectOptions;
  std::filesystem::path package;
  std::filesystem::path outputPath;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--json") {
      json = true;
      continue;
    }
    if ((argument == "--index" || argument == "--search-path" || argument == "--project" ||
         argument == "--output") && i + 1 >= argc) {
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
  if (fileCount == 0) {
    std::cerr << "Usage: DelphiAstTool --project package.dpk --output package.ast.json "
                 "[--index file.jdi] [--search-path dir]\n"
                 "       DelphiAstTool [--json] unit1.pas [unit2.pas ...]\n";
    return 2;
  }
  if ((!package.empty() && outputPath.empty()) || (package.empty() && !outputPath.empty())) {
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
      const auto report = outputPath.empty()
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
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
