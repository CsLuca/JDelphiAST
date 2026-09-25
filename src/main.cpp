#include "jdelphiast/analyzer.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
  bool json = false;
  int fileCount = 0;
  jdelphiast::Analyzer analyzer;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--json") {
      json = true;
      continue;
    }
    std::ifstream input(argument, std::ios::binary);
    if (!input) {
      std::cerr << "Cannot read " << argument << '\n';
      return 2;
    }
    analyzer.addSource(argument, {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()});
    ++fileCount;
  }
  if (fileCount == 0) {
    std::cerr << "Usage: jdelphiast_cli [--json] unit1.pas [unit2.pas ...]\n";
    return 2;
  }
  const auto result = analyzer.analyze();
  std::cout << (json ? jdelphiast::toJson(result) : jdelphiast::toText(result));
}
