#include "jdelphiast/project.hpp"
#include "jdelphiast/preprocessor.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>

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

std::string expandProperties(std::string value, const std::unordered_map<std::string, std::string>& properties,
                             bool unknownAsEmpty, std::vector<std::string>& diagnostics) {
  for (int pass = 0; pass < 32; ++pass) {
    const auto begin = value.find("$(");
    if (begin == std::string::npos) return value;
    const auto end = value.find(')', begin + 2);
    if (end == std::string::npos) break;
    const auto name = lower(value.substr(begin + 2, end - begin - 2));
    const auto found = properties.find(name);
    if (found == properties.end()) {
      if (!unknownAsEmpty) {
        diagnostics.push_back("Unresolved MSBuild property $(" + name + ")");
        return value;
      }
      value.replace(begin, end - begin + 1, "");
    } else value.replace(begin, end - begin + 1, found->second);
  }
  diagnostics.push_back("MSBuild property expansion exceeded limit: " + value);
  return value;
}

bool evaluateComparison(std::string expression, const std::unordered_map<std::string, std::string>& properties,
                        bool& known, std::vector<std::string>& diagnostics) {
  expression = trim(expression);
  while (expression.size() >= 2 && expression.front() == '(' && expression.back() == ')')
    expression = trim(expression.substr(1, expression.size() - 2));
  std::size_t macro = 0;
  while ((macro = expression.find("$(", macro)) != std::string::npos) {
    const auto end = expression.find(')', macro + 2);
    if (end == std::string::npos || !properties.contains(lower(expression.substr(macro + 2, end - macro - 2)))) {
      known = false;
      return false;
    }
    macro = end + 1;
  }
  expression = trim(expandProperties(std::move(expression), properties, false, diagnostics));
  const auto equal = expression.find("==");
  const auto unequal = expression.find("!=");
  const auto op = equal != std::string::npos ? equal : unequal;
  if (op == std::string::npos) { known = false; return false; }
  auto left = trim(expression.substr(0, op));
  auto right = trim(expression.substr(op + 2));
  const auto unquote = [](std::string value) {
    if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') ||
                              (value.front() == '"' && value.back() == '"')))
      return value.substr(1, value.size() - 2);
    return value;
  };
  left = unquote(left);
  right = unquote(right);
  known = true;
  const auto same = lower(left) == lower(right);
  return equal != std::string::npos ? same : !same;
}

bool evaluateCondition(std::string condition, const std::unordered_map<std::string, std::string>& properties,
                       bool& known, std::vector<std::string>& diagnostics) {
  condition = trim(condition);
  if (condition.empty()) { known = true; return true; }
  const auto lowerCondition = lower(condition);
  const auto orAt = lowerCondition.find(" or ");
  if (orAt != std::string::npos) {
    bool leftKnown = true, rightKnown = true;
    const auto left = evaluateCondition(condition.substr(0, orAt), properties, leftKnown, diagnostics);
    const auto right = evaluateCondition(condition.substr(orAt + 4), properties, rightKnown, diagnostics);
    known = leftKnown && rightKnown;
    return left || right;
  }
  const auto andAt = lowerCondition.find(" and ");
  if (andAt != std::string::npos) {
    bool leftKnown = true, rightKnown = true;
    const auto left = evaluateCondition(condition.substr(0, andAt), properties, leftKnown, diagnostics);
    const auto right = evaluateCondition(condition.substr(andAt + 5), properties, rightKnown, diagnostics);
    known = leftKnown && rightKnown;
    return left && right;
  }
  return evaluateComparison(condition, properties, known, diagnostics);
}

std::string attribute(std::string_view tag, std::string_view name) {
  auto at = lower(std::string(tag)).find(lower(std::string(name)) + "=");
  if (at == std::string::npos) return {};
  at += name.size() + 1;
  while (at < tag.size() && std::isspace(static_cast<unsigned char>(tag[at]))) ++at;
  if (at >= tag.size() || (tag[at] != '\'' && tag[at] != '"')) return {};
  const auto quote = tag[at++];
  const auto end = tag.find(quote, at);
  return end == std::string_view::npos ? std::string{} : xmlDecode(std::string(tag.substr(at, end - at)));
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
  const auto directory = path.parent_path();
  std::unordered_map<std::string, std::string> properties;
  properties["projectdir"] = directory.string();
  properties["projectpath"] = path.string();
  properties["config"] = options.configuration;
  properties["platform"] = options.platform;
  std::size_t at = 0;
  while ((at = lower(xml).find("<propertygroup", at)) != std::string::npos) {
    const auto tagEnd = xml.find('>', at);
    const auto groupEnd = lower(xml).find("</propertygroup>", tagEnd);
    if (tagEnd == std::string::npos || groupEnd == std::string::npos) {
      diagnostics.push_back("Malformed PropertyGroup in " + path.string());
      break;
    }
    const auto groupTag = std::string_view(xml).substr(at, tagEnd - at + 1);
    bool known = true;
    if (evaluateCondition(attribute(groupTag, "Condition"), properties, known, diagnostics)) {
      const auto body = xml.substr(tagEnd + 1, groupEnd - tagEnd - 1);
      std::size_t propertyAt = 0;
      while ((propertyAt = body.find('<', propertyAt)) != std::string::npos) {
        if (propertyAt + 1 >= body.size() || body[propertyAt + 1] == '/' || body[propertyAt + 1] == '!') { ++propertyAt; continue; }
        const auto propertyTagEnd = body.find('>', propertyAt);
        if (propertyTagEnd == std::string::npos) break;
        const auto nameEnd = body.find_first_of(" \t/>\r\n", propertyAt + 1);
        if (nameEnd == std::string::npos || nameEnd > propertyTagEnd) break;
        const auto name = body.substr(propertyAt + 1, nameEnd - propertyAt - 1);
        const auto closeTag = "</" + name + ">";
        const auto valueEnd = lower(body).find(lower(closeTag), propertyTagEnd + 1);
        if (valueEnd == std::string::npos) { propertyAt = propertyTagEnd + 1; continue; }
        const auto tag = std::string_view(body).substr(propertyAt, propertyTagEnd - propertyAt + 1);
        bool propertyKnown = true;
        if (evaluateCondition(attribute(tag, "Condition"), properties, propertyKnown, diagnostics)) {
          const auto key = lower(name);
          if (key != "config" && key != "platform")
            properties[key] = expandProperties(xmlDecode(body.substr(propertyTagEnd + 1, valueEnd - propertyTagEnd - 1)),
                                               properties, false, diagnostics);
        }
        if (!propertyKnown) diagnostics.push_back("Unsupported DPROJ property condition: " + attribute(tag, "Condition"));
        propertyAt = valueEnd + closeTag.size();
      }
    }
    if (!known) diagnostics.push_back("Unsupported DPROJ group condition: " + attribute(groupTag, "Condition"));
    at = groupEnd + 16;
  }
  const auto addPaths = [&](std::string_view property, std::vector<std::filesystem::path>& destination) {
    const auto found = properties.find(std::string(property));
    if (found == properties.end()) return;
    for (auto item : split(found->second, ';')) {
      if (item.find("$(") != std::string::npos) { diagnostics.push_back("Unresolved DPROJ macro in path: " + item); continue; }
      auto resolved = delphiPath(item);
      if (resolved.is_relative()) resolved = directory / resolved;
      destination.push_back(resolved.lexically_normal());
    }
  };
  addPaths("dcc_unitsearchpath", options.searchPaths);
  addPaths("dcc_includepath", options.includePaths);
  if (const auto found = properties.find("dcc_namespace"); found != properties.end()) {
    const auto values = split(found->second, ';');
    options.namespaces.insert(options.namespaces.end(), values.begin(), values.end());
  }
  if (const auto found = properties.find("dcc_define"); found != properties.end()) {
    const auto values = split(found->second, ';');
    options.defines.insert(options.defines.end(), values.begin(), values.end());
  }
  /* Legacy fallback for minimal DPROJ files without PropertyGroup parsing. */
  if (properties.empty()) for (const auto& value : xmlValues(xml, "DCC_UnitSearchPath")) {
    for (auto item : split(value, ';')) {
      item = expandProjectMacros(std::move(item), directory, diagnostics);
      if (item.find("$(") == std::string::npos) {
        auto resolved = delphiPath(item);
        if (resolved.is_relative()) resolved = directory / resolved;
        options.searchPaths.push_back(resolved.lexically_normal());
      }
    }
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
    if (fields.size() > 3) {
      const auto decode = [](std::string value) {
        const auto hex = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
          return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        };
        std::string result;
        for (std::size_t i = 0; i < value.size(); ++i) {
          if (value[i] == '%' && i + 2 < value.size() && hex(value[i + 1]) >= 0 && hex(value[i + 2]) >= 0) {
            result += static_cast<char>((hex(value[i + 1]) << 4) | hex(value[i + 2]));
            i += 2;
          } else result += value[i];
        }
        return result;
      };
      for (const auto& encoded : split(fields[3], ';')) {
        const auto parts = split(encoded, ',');
        if (parts.size() >= 2) {
          AstDeclaration declaration;
          declaration.kind = decode(parts[0]);
          declaration.name = decode(parts[1]);
          declaration.visibility = "public";
          if (parts.size() >= 3) declaration.type = decode(parts[2]);
          unit.declarations.push_back(std::move(declaration));
        }
      }
    }
    result.push_back(std::move(unit));
  }
  return result;
}

std::string createSymbolIndex(const std::vector<std::filesystem::path>& sources) {
  std::vector<std::filesystem::path> files;
  for (const auto& source : sources) {
    if (std::filesystem::is_regular_file(source)) files.push_back(source);
    else if (std::filesystem::is_directory(source)) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
        if (!entry.is_regular_file()) continue;
        if (lower(entry.path().extension().string()) == ".pas") files.push_back(entry.path());
      }
    } else throw std::runtime_error("Index source not found: " + source.string());
  }
  std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
    return lower(left.string()) < lower(right.string());
  });
  files.erase(std::unique(files.begin(), files.end()), files.end());
  std::vector<UnitAst> units;
  std::unordered_set<std::string> names;
  for (const auto& file : files) {
    auto ast = parseUnit(file, readFile(file));
    if (ast.name.empty()) throw std::runtime_error("No Delphi unit declaration in " + file.string());
    if (!names.insert(lower(ast.name)).second) throw std::runtime_error("Duplicate indexed unit: " + ast.name);
    units.push_back(std::move(ast));
  }
  if (units.empty()) throw std::runtime_error("No Pascal source files found for index");
  std::sort(units.begin(), units.end(), [](const UnitAst& left, const UnitAst& right) {
    return lower(left.name) < lower(right.name);
  });
  const auto encode = [](std::string_view value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : value) {
      if (std::isalnum(c) || c == '_' || c == '.' || c == '-') result += static_cast<char>(c);
      else { result += '%'; result += digits[c >> 4]; result += digits[c & 15]; }
    }
    return result;
  };
  std::ostringstream output;
  output << "# JDelphiAST symbol index v2: unit|symbols|flags|declarations\n";
  for (const auto& unit : units) {
    output << unit.name << '|';
    std::unordered_set<std::string> parameterNames;
    for (const auto& declaration : unit.declarations)
      for (const auto& parameter : declaration.parameters) parameterNames.insert(lower(parameter.name));
    bool firstSymbol = true;
    for (const auto& symbol : unit.exports) {
      if (parameterNames.contains(lower(symbol.name))) continue;
      if (!firstSymbol) output << ',';
      firstSymbol = false;
      output << symbol.name;
    }
    output << '|';
    bool flag = false;
    if (unit.hasInitialization) { output << "initialization"; flag = true; }
    if (unit.hasFinalization) { if (flag) output << ','; output << "finalization"; }
    output << '|';
    bool firstDeclaration = true;
    for (const auto& declaration : unit.declarations) {
      if (declaration.visibility != "public") continue;
      if (!firstDeclaration) output << ';';
      firstDeclaration = false;
      std::ostringstream signature;
      signature << declaration.kind << ' ' << declaration.name;
      if (!declaration.parameters.empty()) {
        signature << '(';
        for (std::size_t p = 0; p < declaration.parameters.size(); ++p) {
          if (p) signature << "; ";
          signature << declaration.parameters[p].name << ": " << declaration.parameters[p].type;
        }
        signature << ')';
      }
      if (!declaration.type.empty()) signature << ": " << declaration.type;
      output << encode(declaration.kind) << ',' << encode(declaration.name) << ','
             << encode(declaration.type) << ',' << encode(signature.str());
    }
    output << '\n';
  }
  return output.str();
}

ProjectLoadResult loadPackage(const std::filesystem::path& packageFile, ProjectOptions options) {
  ProjectLoadResult result;
  result.options = std::move(options);
  const auto package = std::filesystem::absolute(packageFile).lexically_normal();
  if (!std::filesystem::exists(package)) throw std::runtime_error("Package not found: " + package.string());
  const auto packageDir = package.parent_path();
  result.options.searchPaths.insert(result.options.searchPaths.begin(), packageDir);
  const auto dproj = result.options.dprojFile.empty()
      ? package.parent_path() / (package.stem().string() + ".dproj")
      : result.options.dprojFile;
  loadDproj(dproj, result.options, result.diagnostics);
  if (!result.diagnostics.empty()) result.analyzer.setEnvironmentComplete(false);
  const auto addDefine = [&](std::string define) {
    if (std::none_of(result.options.defines.begin(), result.options.defines.end(), [&](const std::string& existing) {
          return lower(existing) == lower(define);
        })) result.options.defines.push_back(std::move(define));
  };
  addDefine(result.options.configuration);
  if (lower(result.options.platform) == "win32") { addDefine("MSWINDOWS"); addDefine("WIN32"); addDefine("CPUX86"); }
  else if (lower(result.options.platform) == "win64") { addDefine("MSWINDOWS"); addDefine("WIN64"); addDefine("CPUX64"); }
  result.analyzer.setPreprocessor(result.options.defines, result.options.includePaths);
  result.analyzer.setBuildContext(result.options.configuration, result.options.platform);

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
    const auto preprocessed = preprocess(path, source, {result.options.defines, result.options.includePaths, 64});
    const auto ast = parseUnit(path, preprocessed.source);
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
