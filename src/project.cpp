#include "jdelphiast/project.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace jdelphiast {
namespace {

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::vector<std::string> split(std::string_view value, char delimiter) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find(delimiter, start);
    auto item = trim(std::string(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start)));
    if (!item.empty()) result.push_back(std::move(item));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return result;
}

std::vector<std::string> splitFields(std::string_view value) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find('|', start);
    result.push_back(trim(std::string(value.substr(
        start, end == std::string_view::npos ? value.size() - start : end - start))));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return result;
}

std::filesystem::path delphiPath(std::string value) {
  // Forward slashes are accepted by std::filesystem on Windows and POSIX.
  std::replace(value.begin(), value.end(), '\\', '/');
  return std::filesystem::path(value);
}

std::string xmlDecode(std::string value) {
  const std::pair<std::string_view, std::string_view> entities[] = {
      {"&amp;", "&"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}};
  for (const auto& [encoded, decoded] : entities) {
    std::size_t at = 0;
    while ((at = value.find(encoded, at)) != std::string::npos) {
      value.replace(at, encoded.size(), decoded);
      at += decoded.size();
    }
  }
  return value;
}

std::vector<std::string> xmlValues(const std::string& xml, std::string_view tag) {
  std::vector<std::string> result;
  const auto open = "<" + std::string(tag);
  const auto close = "</" + std::string(tag) + ">";
  std::size_t at = 0;
  while ((at = xml.find(open, at)) != std::string::npos) {
    const auto start = xml.find('>', at + open.size());
    if (start == std::string::npos) break;
    const auto end = xml.find(close, start + 1);
    if (end == std::string::npos) break;
    result.push_back(xmlDecode(xml.substr(start + 1, end - start - 1)));
    at = end + close.size();
  }
  return result;
}

std::string expandProjectMacros(std::string value, const std::filesystem::path& projectDir,
                                std::vector<std::string>& diagnostics) {
  const std::pair<std::string_view, std::string> known[] = {
      {"$(PROJECTDIR)", projectDir.string()}, {"$(PROJECTPATH)", projectDir.string()}};
  for (const auto& [macro, replacement] : known) {
    std::size_t at = 0;
    while ((at = lower(value).find(lower(std::string(macro)), at)) != std::string::npos) {
      value.replace(at, macro.size(), replacement);
      at += replacement.size();
    }
  }
  if (value.find("$(") != std::string::npos)
    diagnostics.push_back("Unresolved DPROJ macro in path: " + value);
  return value;
}

void loadDproj(const std::filesystem::path& path, ProjectOptions& options,
               std::vector<std::string>& diagnostics) {
  if (!std::filesystem::exists(path)) return;
  const auto xml = readFile(path);
  if (lower(xml).find("condition=") != std::string::npos)
    diagnostics.push_back("Conditional DPROJ properties were merged; analysis is conservative");
  const auto directory = path.parent_path();
  for (const auto& value : xmlValues(xml, "DCC_UnitSearchPath")) {
    for (auto item : split(value, ';')) {
      item = expandProjectMacros(std::move(item), directory, diagnostics);
      if (item.find("$(") == std::string::npos) {
        auto resolved = delphiPath(item);
        if (resolved.is_relative()) resolved = directory / resolved;
        options.searchPaths.push_back(resolved.lexically_normal());
      }
    }
  }
  for (const auto& value : xmlValues(xml, "DCC_Namespace")) {
    const auto values = split(value, ';');
    options.namespaces.insert(options.namespaces.end(), values.begin(), values.end());
  }
  for (const auto& value : xmlValues(xml, "DCC_Define")) {
    const auto values = split(value, ';');
    options.defines.insert(options.defines.end(), values.begin(), values.end());
  }
}

struct PackageToken {
  enum class Kind { Identifier, String, Symbol } kind;
  std::string text;
};

std::vector<PackageToken> packageTokens(const std::string& source) {
  std::vector<PackageToken> result;
  std::size_t i = 0;
  while (i < source.size()) {
    if (std::isspace(static_cast<unsigned char>(source[i]))) { ++i; continue; }
    if (source.compare(i, 2, "//") == 0) {
      while (i < source.size() && source[i] != '\n') ++i;
      continue;
    }
    if (source[i] == '{') {
      const auto end = source.find('}', i + 1);
      i = end == std::string::npos ? source.size() : end + 1;
      continue;
    }
    if (source.compare(i, 2, "(*") == 0) {
      const auto end = source.find("*)", i + 2);
      i = end == std::string::npos ? source.size() : end + 2;
      continue;
    }
    if (source[i] == '\'') {
      ++i;
      std::string value;
      while (i < source.size()) {
        if (source[i] != '\'') value += source[i++];
        else if (i + 1 < source.size() && source[i + 1] == '\'') { value += '\''; i += 2; }
        else { ++i; break; }
      }
      result.push_back({PackageToken::Kind::String, std::move(value)});
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(source[i])) || source[i] == '_') {
      const auto start = i++;
      while (i < source.size() && (std::isalnum(static_cast<unsigned char>(source[i])) ||
                                   source[i] == '_' || source[i] == '.')) ++i;
      result.push_back({PackageToken::Kind::Identifier, source.substr(start, i - start)});
      continue;
    }
    result.push_back({PackageToken::Kind::Symbol, std::string(1, source[i++])});
  }
  return result;
}

std::vector<std::filesystem::path> packageSources(const std::string& source) {
  std::vector<std::filesystem::path> result;
  const auto tokens = packageTokens(source);
  std::size_t i = 0;
  while (i < tokens.size() && !(tokens[i].kind == PackageToken::Kind::Identifier &&
                                lower(tokens[i].text) == "contains")) ++i;
  if (i == tokens.size()) return result;
  for (++i; i < tokens.size() && tokens[i].text != ";";) {
    if (tokens[i].kind != PackageToken::Kind::Identifier) { ++i; continue; }
    const auto unitName = tokens[i++].text;
    if (i + 1 < tokens.size() && tokens[i].kind == PackageToken::Kind::Identifier &&
        lower(tokens[i].text) == "in" && tokens[i + 1].kind == PackageToken::Kind::String) {
      result.push_back(delphiPath(tokens[i + 1].text));
      i += 2;
    } else {
      result.push_back(delphiPath(unitName + ".pas"));
    }
    if (i < tokens.size() && tokens[i].text == ",") ++i;
  }
  return result;
}

struct FoundUnit { std::filesystem::path path; std::string candidateName; };

std::optional<FoundUnit> findUnit(const std::string& name,
                                  const std::vector<std::filesystem::path>& searchPaths,
                                  const std::vector<std::string>& namespaces) {
  std::vector<std::string> candidates{name};
  if (name.find('.') == std::string::npos)
    for (const auto& scope : namespaces) candidates.push_back(scope + "." + name);
  for (const auto& directory : searchPaths) {
    if (!std::filesystem::exists(directory)) continue;
    for (const auto& candidate : candidates) {
      const auto exact = directory / (candidate + ".pas");
      if (std::filesystem::exists(exact)) return FoundUnit{exact, candidate};
      const auto flat = directory / (candidate.substr(candidate.find_last_of('.') + 1) + ".pas");
      if (std::filesystem::exists(flat)) return FoundUnit{flat, candidate};
      for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && lower(entry.path().filename().string()) == lower(candidate + ".pas"))
          return FoundUnit{entry.path(), candidate};
      }
    }
  }
  return std::nullopt;
}

}  // namespace

std::vector<IndexedUnit> loadSymbolIndex(const std::filesystem::path& indexFile) {
  std::vector<IndexedUnit> result;
  std::istringstream input(readFile(indexFile));
  std::string line;
  while (std::getline(input, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    const auto fields = splitFields(line);
    if (fields.empty()) continue;
    IndexedUnit unit;
    unit.name = fields[0];
    if (fields.size() > 1) unit.symbols = split(fields[1], ',');
    if (fields.size() > 2) {
      for (const auto& flag : split(fields[2], ',')) {
        if (lower(flag) == "initialization") unit.hasInitialization = true;
        if (lower(flag) == "finalization") unit.hasFinalization = true;
        if (lower(flag) == "complete") unit.complete = true;
      }
    }
    result.push_back(std::move(unit));
  }
  return result;
}

ProjectLoadResult loadPackage(const std::filesystem::path& packageFile, ProjectOptions options) {
  ProjectLoadResult result;
  result.options = std::move(options);
  const auto package = std::filesystem::absolute(packageFile).lexically_normal();
  if (!std::filesystem::exists(package)) throw std::runtime_error("Package not found: " + package.string());
  const auto packageDir = package.parent_path();
  result.options.searchPaths.insert(result.options.searchPaths.begin(), packageDir);
  loadDproj(package.parent_path() / (package.stem().string() + ".dproj"), result.options, result.diagnostics);
  if (std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const std::string& message) {
        return message.find("Conditional DPROJ") != std::string::npos ||
               message.find("Unresolved DPROJ macro") != std::string::npos;
      }))
    result.analyzer.setEnvironmentComplete(false);

  for (const auto& index : result.options.indexFiles) {
    for (auto unit : loadSymbolIndex(index)) {
      for (const auto& scope : result.options.namespaces) {
        const auto prefix = scope + ".";
        if (lower(unit.name).starts_with(lower(prefix)))
          result.analyzer.addUnitAlias(unit.name.substr(prefix.size()), unit.name);
      }
      result.analyzer.addIndexedUnit(std::move(unit));
    }
  }

  const auto dpkSource = readFile(package);
  std::vector<std::filesystem::path> pending;
  for (auto relative : packageSources(dpkSource)) {
    if (relative.is_relative()) relative = packageDir / relative;
    pending.push_back(relative.lexically_normal());
  }
  if (pending.empty()) result.diagnostics.push_back("No source units found in package contains clause");

  std::unordered_set<std::string> loaded;
  while (!pending.empty()) {
    auto path = std::filesystem::absolute(pending.back()).lexically_normal();
    pending.pop_back();
    const auto key = lower(path.string());
    if (loaded.contains(key)) continue;
    if (!std::filesystem::exists(path)) {
      result.diagnostics.push_back("Source not found: " + path.string());
      continue;
    }
    const auto source = readFile(path);
    const auto ast = parseUnit(path, source);
    result.analyzer.addSource(path, source);
    result.sourceFiles.push_back(path);
    loaded.insert(key);
    for (const auto& use : ast.uses) {
      if (const auto found = findUnit(use.name, result.options.searchPaths, result.options.namespaces)) {
        if (lower(found->candidateName) != lower(use.name)) result.analyzer.addUnitAlias(use.name, found->candidateName);
        if (!loaded.contains(lower(std::filesystem::absolute(found->path).lexically_normal().string())))
          pending.push_back(found->path);
      }
    }
  }
  return result;
}

}  // namespace jdelphiast
