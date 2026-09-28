#include "jdelphiast/project.hpp"
#include "jdelphiast/preprocessor.hpp"
#include "jdelphiast/path.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>

namespace jdelphiast {
namespace {

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read " + pathToUtf8(path));
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

std::vector<std::string> splitPreservingEmpty(std::string_view value, char delimiter) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find(delimiter, start);
    result.push_back(trim(std::string(value.substr(start,
        end == std::string_view::npos ? value.size() - start : end - start))));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return result;
}

std::string percentDecode(std::string value) {
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
}

std::filesystem::path delphiPath(std::string value) {
  // Forward slashes are accepted by std::filesystem on Windows and POSIX.
  std::replace(value.begin(), value.end(), '\\', '/');
  return pathFromSourceBytes(value);
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
          return value;
        }
      value.replace(begin, end - begin + 1, "");
    } else value.replace(begin, end - begin + 1, found->second);
  }
  diagnostics.push_back("MSBuild property expansion exceeded limit: " + value);
  return value;
}

class ConditionParser {
 public:
  ConditionParser(std::string_view input, const std::unordered_map<std::string, std::string>& properties)
      : input_(input), properties_(properties) {}

  bool parse(bool& known) {
    known = true;
    skipSpaces();
    if (atEnd()) return true;
    const auto value = parseOr(known);
    skipSpaces();
    if (!atEnd()) known = false;
    return value;
  }

 private:
  bool parseOr(bool& known) {
    auto value = parseAnd(known);
    while (known && consumeWord("or")) value = parseAnd(known) || value;
    return value;
  }

  bool parseAnd(bool& known) {
    auto value = parsePrimary(known);
    while (known && consumeWord("and")) value = parsePrimary(known) && value;
    return value;
  }

  bool parsePrimary(bool& known) {
    skipSpaces();
    if (consume('(')) {
      const auto value = parseOr(known);
      if (!consume(')')) known = false;
      return value;
    }
    const auto left = parseOperand(known);
    if (!known) return false;
    skipSpaces();
    const bool equal = consumeText("==");
    const bool unequal = !equal && consumeText("!=");
    if (!equal && !unequal) { known = false; return false; }
    const auto right = parseOperand(known);
    const auto same = lower(left) == lower(right);
    return equal ? same : !same;
  }

  std::string parseOperand(bool& known) {
    skipSpaces();
    if (atEnd()) { known = false; return {}; }
    if (input_[position_] == '\'' || input_[position_] == '"') {
      const auto quote = input_[position_++];
      std::string value;
      while (!atEnd() && input_[position_] != quote) value += input_[position_++];
      if (!consume(quote)) known = false;
      return expand(value);
    }
    const auto start = position_;
    while (!atEnd() && !std::isspace(static_cast<unsigned char>(input_[position_])) &&
           input_[position_] != ')' && input_[position_] != '=' && input_[position_] != '!') ++position_;
    return expand(std::string(input_.substr(start, position_ - start)));
  }

  std::string expand(std::string value) const {
    std::size_t at = 0;
    while ((at = value.find("$(", at)) != std::string::npos) {
      const auto end = value.find(')', at + 2);
      if (end == std::string::npos) return value;
      const auto found = properties_.find(lower(value.substr(at + 2, end - at - 2)));
      const auto replacement = found == properties_.end() ? std::string{} : found->second;
      value.replace(at, end - at + 1, replacement);
      at += replacement.size();
    }
    return value;
  }

  bool consumeWord(std::string_view word) {
    skipSpaces();
    if (position_ + word.size() > input_.size() ||
        lower(std::string(input_.substr(position_, word.size()))) != word) return false;
    const auto end = position_ + word.size();
    if (end < input_.size() && std::isalnum(static_cast<unsigned char>(input_[end]))) return false;
    position_ = end;
    return true;
  }

  bool consumeText(std::string_view text) {
    skipSpaces();
    if (input_.substr(position_, text.size()) != text) return false;
    position_ += text.size();
    return true;
  }

  bool consume(char value) {
    skipSpaces();
    if (atEnd() || input_[position_] != value) return false;
    ++position_;
    return true;
  }

  void skipSpaces() {
    while (!atEnd() && std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_;
  }
  bool atEnd() const { return position_ >= input_.size(); }

  std::string_view input_;
  const std::unordered_map<std::string, std::string>& properties_;
  std::size_t position_{};
};

bool evaluateCondition(const std::string& condition,
                       const std::unordered_map<std::string, std::string>& properties,
                       bool& known, std::vector<std::string>&) {
  return ConditionParser(condition, properties).parse(known);
}

std::string attribute(std::string_view tag, std::string_view name) {
  const auto lowered = lower(std::string(tag));
  auto at = lowered.find(lower(std::string(name)));
  if (at == std::string::npos) return {};
  at += name.size();
  while (at < tag.size() && std::isspace(static_cast<unsigned char>(tag[at]))) ++at;
  if (at >= tag.size() || tag[at] != '=') return {};
  ++at;
  while (at < tag.size() && std::isspace(static_cast<unsigned char>(tag[at]))) ++at;
  if (at >= tag.size() || (tag[at] != '\'' && tag[at] != '"')) return {};
  const auto quote = tag[at++];
  const auto end = tag.find(quote, at);
  return end == std::string_view::npos ? std::string{} : xmlDecode(std::string(tag.substr(at, end - at)));
}

std::string expandProjectMacros(std::string value, const std::filesystem::path& projectDir,
                                std::vector<std::string>& diagnostics) {
  const std::pair<std::string_view, std::string> known[] = {
      {"$(PROJECTDIR)", pathToUtf8(projectDir)}, {"$(PROJECTPATH)", pathToUtf8(projectDir)}};
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
  properties["projectdir"] = pathToUtf8(directory);
  properties["projectpath"] = pathToUtf8(path);
  properties["config"] = options.configuration;
  properties["platform"] = options.platform;
  std::size_t at = 0;
  while ((at = lower(xml).find("<propertygroup", at)) != std::string::npos) {
    const auto tagEnd = xml.find('>', at);
    const auto groupEnd = lower(xml).find("</propertygroup>", tagEnd);
    if (tagEnd == std::string::npos || groupEnd == std::string::npos) {
      diagnostics.push_back("Malformed PropertyGroup in " + pathToUtf8(path));
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
          if (key != "config" && key != "platform") {
            auto expansionProperties = properties;
            if (!expansionProperties.contains(key)) expansionProperties[key] = "";
            properties[key] = expandProperties(
                xmlDecode(body.substr(propertyTagEnd + 1, valueEnd - propertyTagEnd - 1)),
                expansionProperties, false, diagnostics);
          }
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
    if (found->second.find("$(") != std::string::npos)
      diagnostics.push_back("Unresolved DPROJ macro in DCC_Namespace: " + found->second);
    const auto values = split(found->second, ';');
    options.namespaces.insert(options.namespaces.end(), values.begin(), values.end());
  }
  if (const auto found = properties.find("dcc_define"); found != properties.end()) {
    if (found->second.find("$(") != std::string::npos)
      diagnostics.push_back("Unresolved DPROJ macro in DCC_Define: " + found->second);
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
        if (entry.is_regular_file() && lower(pathToUtf8(entry.path().filename())) == lower(candidate + ".pas"))
          return FoundUnit{entry.path(), candidate};
      }
    }
  }
  return std::nullopt;
}

}  // namespace

ValidationPolicy loadValidationPolicy(const std::filesystem::path& policyFile) {
  ValidationPolicy policy;
  if (policyFile.empty()) return policy;
  std::istringstream input(readFile(policyFile));
  std::string line;
  while (std::getline(input, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    const auto equal = line.find('=');
    if (equal == std::string::npos) throw std::runtime_error("Invalid validation policy line: " + line);
    const auto code = lower(trim(line.substr(0, equal)));
    const auto severity = lower(trim(line.substr(equal + 1)));
    if (severity != "info" && severity != "warning" && severity != "error")
      throw std::runtime_error("Invalid validation severity for " + code + ": " + severity);
    policy.severities[code] = severity;
  }
  return policy;
}

std::string validationSeverity(std::string_view code, const ValidationPolicy& policy) {
  const auto normalized = lower(std::string(code));
  if (const auto found = policy.severities.find(normalized); found != policy.severities.end()) return found->second;
  static const std::unordered_set<std::string> infos = {"output_not_written"};
  static const std::unordered_set<std::string> warnings = {
      "no_unit_declaration", "source_unit_not_indexed", "seed_without_export_evidence", "seed_missing_exports"};
  if (infos.contains(normalized)) return "info";
  return warnings.contains(normalized) ? "warning" : "error";
}

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
    unit.indexVersion = "v600";
    if (fields.size() > 1) unit.symbols = split(fields[1], ',');
    if (fields.size() > 2) {
      for (const auto& flag : split(fields[2], ',')) {
        if (lower(flag) == "initialization") unit.hasInitialization = true;
        if (lower(flag) == "finalization") unit.hasFinalization = true;
        if (lower(flag) == "complete") unit.complete = true;
      }
    }
    if (fields.size() > 3) {
      const auto decode = [](std::string value) { return percentDecode(std::move(value)); };
      for (const auto& encoded : split(fields[3], ';')) {
        const auto parts = splitPreservingEmpty(encoded, ',');
        if (parts.size() >= 2) {
          AstDeclaration declaration;
          declaration.kind = decode(parts[0]);
          declaration.name = decode(parts[1]);
          declaration.visibility = "public";
          if (parts.size() >= 3) declaration.type = decode(parts[2]);
          if (parts.size() >= 4) {
            declaration.signature = decode(parts[3]);
            const auto open = declaration.signature.find('(');
            const auto close = declaration.signature.rfind(')');
            if (open != std::string::npos && close != std::string::npos && close > open) {
              for (auto parameterText : split(declaration.signature.substr(open + 1, close - open - 1), ';')) {
                const auto colon = parameterText.find(':');
                if (colon == std::string::npos) continue;
                AstParameter parameter;
                auto nameText = trim(parameterText.substr(0, colon));
                const auto space = nameText.find(' ');
                if (space != std::string::npos) {
                  const auto possibleModifier = lower(nameText.substr(0, space));
                  if (possibleModifier == "const" || possibleModifier == "var" || possibleModifier == "out") {
                    parameter.modifier = possibleModifier;
                    nameText = trim(nameText.substr(space + 1));
                  }
                }
                parameter.name = nameText;
                parameter.type = trim(parameterText.substr(colon + 1));
                declaration.parameters.push_back(std::move(parameter));
              }
            }
          }
          if (parts.size() >= 5) declaration.overload = lower(decode(parts[4])) == "overload";
          if (parts.size() >= 6) declaration.isOverride = lower(decode(parts[5])) == "override";
          const auto ownerSeparator = declaration.name.find_last_of('.');
          if (ownerSeparator != std::string::npos) declaration.ownerType = declaration.name.substr(0, ownerSeparator);
          unit.declarations.push_back(std::move(declaration));
        }
      }
    }
    if (fields.size() > 4) {
      const auto metadata = split(fields[4], ';');
      for (const auto& item : metadata) {
        const auto equal = item.find('=');
        if (equal == std::string::npos) continue;
        const auto key = lower(item.substr(0, equal));
        const auto value = percentDecode(item.substr(equal + 1));
        if (key == "source") unit.sourceFile = delphiPath(value);
        else if (key == "version") unit.indexVersion = value;
        else if (key == "package") unit.packageName = value;
        else if (key == "dcp") unit.packageDcp = value;
        else if (key == "bpl") unit.packageBpl = value;
        else if (key == "project") unit.sourceProject = delphiPath(value);
        else if (key == "origin") unit.origin = value;
      }
    }
    if (fields.size() > 5) {
      for (const auto& encoded : split(fields[5], ';')) {
        const auto parts = splitPreservingEmpty(encoded, ',');
        if (parts.size() < 3) continue;
        unit.inheritance.push_back({parts[0], parts[1], parts[2], {}});
      }
    }
    if (fields.size() > 6) unit.dependencies = split(fields[6], ',');
    result.push_back(std::move(unit));
  }
  return result;
}

IndexLoadResult loadSymbolIndexValidated(const std::filesystem::path& indexFile) {
  IndexLoadResult result;
  std::string source;
  try { source = readFile(indexFile); }
  catch (const std::exception& error) {
    result.hasBlockingErrors = true;
    result.diagnostics.push_back({"index_read_failed", "error", error.what(), indexFile, {}});
    return result;
  }
  std::istringstream input(source);
  std::string line;
  std::unordered_set<std::string> names;
  std::set<std::string> versions;
  std::set<std::string> origins;
  std::size_t lineNumber = 0;
  while (std::getline(input, line)) {
    ++lineNumber;
    line = trim(line);
    if (line.starts_with("# statistics")) {
      const auto failed = line.find("files_parse_failed=");
      if (failed != std::string::npos) {
        const auto valueStart = failed + std::string_view("files_parse_failed=").size();
        const auto valueEnd = line.find(' ', valueStart);
        const auto value = line.substr(valueStart, valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart);
        if (!value.empty() && value != "0") {
        result.hasBlockingErrors = true;
        result.diagnostics.push_back({"index_incomplete", "error",
            "Index metadata reports source parse failures.", indexFile, {}});
        }
      }
      continue;
    }
    if (line.empty() || line[0] == '#') continue;
    const auto fields = splitFields(line);
    if (fields.size() < 3 || fields[0].empty()) {
      result.hasBlockingErrors = true;
      result.diagnostics.push_back({"index_parse_failed", "error",
          "Malformed JDI record at line " + std::to_string(lineNumber), indexFile, {}});
      continue;
    }
    const auto unitName = lower(fields[0]);
    if (!names.insert(unitName).second) {
      result.hasBlockingErrors = true;
      result.diagnostics.push_back({"duplicate_unit_source", "error",
          "Duplicate unit entry in index: " + fields[0], indexFile, fields[0]});
    }
    std::unordered_set<std::string> symbols;
    if (fields.size() > 1) for (const auto& symbol : split(fields[1], ',')) {
      if (!symbols.insert(lower(symbol)).second) {
        result.hasBlockingErrors = true;
        result.diagnostics.push_back({"duplicate_symbol", "error",
            "Duplicate exported symbol in unit " + fields[0] + ": " + symbol, indexFile, fields[0]});
      }
    }
    for (const auto& flag : split(fields[2], ',')) {
      const auto normalized = lower(flag);
      if (normalized != "initialization" && normalized != "finalization" && normalized != "complete") {
        result.hasBlockingErrors = true;
        result.diagnostics.push_back({"index_parse_failed", "error",
            "Unknown JDI flag '" + flag + "' for unit " + fields[0], indexFile, fields[0]});
      }
    }
    if (fields.size() > 4) {
      std::string package, dcp, bpl;
      for (const auto& metadata : split(fields[4], ';')) {
        const auto equal = metadata.find('=');
        if (equal == std::string::npos) continue;
        const auto key = lower(metadata.substr(0, equal));
        const auto value = percentDecode(metadata.substr(equal + 1));
        if (key == "version" && !value.empty() && value != "unknown") versions.insert(lower(value));
        else if (key == "origin" && !value.empty()) origins.insert(lower(value));
        else if (key == "package") package = value;
        else if (key == "dcp") dcp = value;
        else if (key == "bpl") bpl = value;
      }
      if (package.empty() && (!dcp.empty() || !bpl.empty())) {
        result.hasBlockingErrors = true;
        result.diagnostics.push_back({"package_metadata_inconsistent", "error",
            "DCP/BPL metadata requires an explicit package name.", indexFile, fields[0]});
      }
    }
  }
  if (versions.size() > 1) {
    result.hasBlockingErrors = true;
    result.diagnostics.push_back({"catalog_version_inconsistent", "error",
        "Index contains more than one catalog version.", indexFile, {}});
  }
  if (origins.size() > 1) {
    result.hasBlockingErrors = true;
    result.diagnostics.push_back({"catalog_origin_inconsistent", "error",
        "Index contains more than one catalog origin.", indexFile, {}});
  }
  try { result.units = loadSymbolIndex(indexFile); }
  catch (const std::exception& error) {
    result.hasBlockingErrors = true;
    result.diagnostics.push_back({"index_parse_failed", "error", error.what(), indexFile, {}});
  }
  if (result.units.empty() && !result.hasBlockingErrors) {
    result.hasBlockingErrors = true;
    result.diagnostics.push_back({"no_input_units", "error", "Index contains no Delphi units.", indexFile, {}});
  }
  return result;
}

std::vector<IndexedUnit> bundledSymbolIndex() {
  static constexpr std::string_view data = R"(System|TObject,TClass,TInterfacedObject,IInterface,Exception,Integer,Boolean,String|
System.SysUtils|Exception,EAbort,EConvertError,EStreamError,FreeAndNil,Format,IntToStr,StrToInt,SameText,UpperCase,LowerCase|initialization,finalization
System.Classes|TStringList,TStrings,TStream,TFileStream,TMemoryStream,TComponent,TPersistent,TCollection,TList|initialization,finalization
System.Types|TPoint,TRect,TSize|
System.Variants|Variant,VarToStr,VarIsNull|initialization
Vcl.Forms|TForm,TApplication,Application,Screen|initialization,finalization
Vcl.Controls|TControl,TWinControl,TGraphicControl|initialization,finalization
Vcl.Dialogs|TOpenDialog,TSaveDialog,TColorDialog,MessageDlg|initialization
Winapi.Windows|HWND,HANDLE,DWORD,WPARAM,LPARAM,LRESULT,MessageBox|
CSCore.Types|TParam,TCSParam,TCSDate,TCSAmount,CS_Pos_Value|
CSResources.Globals|APPTITLE,CS_Pos_Value|initialization
RSql|TDBExec,TQuery,TParam,ExecSql,OpenQuery|initialization
UCoreDB|DBExec,GetConnection,StartTransaction,Commit,Rollback|initialization
CSCore.Note.Utils|TNoteUtils,UpdateOrInsertSection,DeleteSection,AddSection||class_function,TNoteUtils.UpdateOrInsertSection,Integer,TNoteUtils.UpdateOrInsertSection;class_function,TNoteUtils.DeleteSection,Boolean,TNoteUtils.DeleteSection
)";
  std::vector<IndexedUnit> units;
  std::istringstream input{std::string(data)};
  std::string line;
  while (std::getline(input, line)) {
    const auto fields = splitFields(trim(line));
    if (fields.empty() || fields[0].empty()) continue;
    IndexedUnit unit;
    unit.name = fields[0];
    if (fields.size() > 1) unit.symbols = split(fields[1], ',');
    if (fields.size() > 2) for (const auto& flag : split(fields[2], ',')) {
      if (lower(flag) == "initialization") unit.hasInitialization = true;
      if (lower(flag) == "finalization") unit.hasFinalization = true;
      if (lower(flag) == "complete") unit.complete = true;
    }
    if (fields.size() > 3) for (const auto& encoded : split(fields[3], ';')) {
      const auto parts = splitPreservingEmpty(encoded, ',');
      if (parts.size() < 2) continue;
      AstDeclaration declaration;
      declaration.kind = parts[0];
      declaration.name = parts[1];
      declaration.visibility = "public";
      if (parts.size() > 2) declaration.type = parts[2];
      if (parts.size() > 3) declaration.signature = parts[3];
      unit.declarations.push_back(std::move(declaration));
    }
    units.push_back(std::move(unit));
  }
  return units;
}

std::string createSymbolIndex(const std::vector<std::filesystem::path>& sources) {
  return buildSymbolIndex(sources).content;
}

std::string createSymbolIndex(const std::vector<std::filesystem::path>& sources,
                              std::string_view version) {
  IndexBuildOptions options;
  options.version = std::string(version);
  return buildSymbolIndex(sources, options).content;
}

IndexBuildResult buildSymbolIndex(const std::vector<std::filesystem::path>& sources,
                                  const IndexBuildOptions& options) {
  IndexBuildResult result;
  const auto severityPolicy = loadValidationPolicy(options.validationPolicy);
  const auto severity = [&](std::string_view code, std::string_view fallback) {
    const auto found = severityPolicy.severities.find(lower(std::string(code)));
    return found == severityPolicy.severities.end() ? std::string(fallback) : found->second;
  };
  std::vector<std::filesystem::path> files;
  std::vector<std::filesystem::path> packages;
  auto allSources = sources;
  allSources.insert(allSources.end(), options.sourceRoots.begin(), options.sourceRoots.end());
  for (const auto& source : allSources) {
    if (std::filesystem::is_regular_file(source)) files.push_back(source);
    else if (std::filesystem::is_directory(source)) {
      std::error_code iteratorError;
      std::filesystem::recursive_directory_iterator iterator(
          source, std::filesystem::directory_options::skip_permission_denied, iteratorError);
      for (const auto end = std::filesystem::recursive_directory_iterator(); iterator != end; iterator.increment(iteratorError)) {
        if (iteratorError) {
          const auto level = severity("source_scan_failed", "error");
          result.hasBlockingErrors = result.hasBlockingErrors || level == "error";
          result.diagnostics.push_back({source, "scan_failed", "source_scan_failed", level, iteratorError.message()});
          iteratorError.clear();
          continue;
        }
        const auto& entry = *iterator;
        if (!entry.is_regular_file(iteratorError)) continue;
        const auto extension = lower(pathToUtf8(entry.path().extension()));
        if (extension == ".pas") files.push_back(entry.path());
        else if (extension == ".dpk") packages.push_back(entry.path());
      }
    } else {
      const auto level = severity("missing_source_root", "error");
      result.hasBlockingErrors = result.hasBlockingErrors || level == "error";
      result.diagnostics.push_back({source, "scan_failed", "missing_source_root", level,
                                    "Index source root does not exist."});
    }
  }
  std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
    const auto leftText = pathToUtf8(left), rightText = pathToUtf8(right);
    const auto leftLower = lower(leftText), rightLower = lower(rightText);
    return leftLower == rightLower ? leftText < rightText : leftLower < rightLower;
  });
  files.erase(std::unique(files.begin(), files.end(), [](const auto& left, const auto& right) {
    return lower(pathToUtf8(left)) == lower(pathToUtf8(right));
  }), files.end());
  for (const auto& packageRoot : options.packageRoots)
    if (std::filesystem::is_directory(packageRoot))
      for (const auto& entry : std::filesystem::recursive_directory_iterator(packageRoot))
        if (entry.is_regular_file() && lower(pathToUtf8(entry.path().extension())) == ".dpk") packages.push_back(entry.path());
  std::sort(packages.begin(), packages.end(), [](const auto& left, const auto& right) {
    const auto leftText = pathToUtf8(left), rightText = pathToUtf8(right);
    const auto leftLower = lower(leftText), rightLower = lower(rightText);
    return leftLower == rightLower ? leftText < rightText : leftLower < rightLower;
  });
  packages.erase(std::unique(packages.begin(), packages.end()), packages.end());
  std::vector<UnitAst> units;
  struct PackageMetadata { std::string name, dcp, bpl; std::filesystem::path project; };
  std::unordered_map<std::string, PackageMetadata> packageByFile;
  for (const auto& package : packages) {
    try {
      const auto packageName = pathToUtf8(package.stem());
      for (auto relative : packageSources(readFile(package))) {
        if (relative.is_relative()) relative = package.parent_path() / relative;
        packageByFile[lower(pathToUtf8(std::filesystem::absolute(relative).lexically_normal()))] = {
            packageName, packageName + ".dcp", packageName + ".bpl", package};
      }
    } catch (const std::filesystem::filesystem_error&) {
      const auto level = severity("filesystem_path_encoding_error", "warning");
      result.hasBlockingErrors = result.hasBlockingErrors || level == "error";
      result.diagnostics.push_back({package, "path_encoding_error", "filesystem_path_encoding_error", level,
                                    "A package path could not be converted; package metadata was skipped."});
    }
  }
  std::unordered_set<std::string> names;
  for (const auto& file : files) {
    ++result.statistics.filesScanned;
    UnitAst ast;
    std::string sourceText;
    try { sourceText = readFile(file); }
    catch (const std::exception& error) {
      ++result.statistics.filesParseFailed;
      const auto level = severity("file_read_error", "error");
      result.hasBlockingErrors = result.hasBlockingErrors || level == "error";
      result.diagnostics.push_back({file, "read_failed", "file_read_error", level, error.what()});
      continue;
    }
    try { ast = parseUnit(file, sourceText); }
    catch (const std::exception& error) {
      ++result.statistics.filesParseFailed;
      const auto level = severity("parse_failure", "error");
      result.hasBlockingErrors = result.hasBlockingErrors || level == "error";
      result.diagnostics.push_back({file, "parse_failed", "parse_failure", level, error.what()});
      continue;
    }
    if (ast.name.empty()) {
      ++result.statistics.filesSkippedNonUnitSource;
      const auto level = severity("no_unit_declaration", "warning");
      result.hasBlockingErrors = result.hasBlockingErrors || level == "error";
      result.diagnostics.push_back({file, "skipped_non_unit_source", "no_unit_declaration", level,
                                    "The Pascal file does not contain a parseable unit declaration."});
      continue;
    }
    if (!ast.complete) {
      ++result.statistics.filesParseFailed;
      const auto level = severity("incomplete_unit", "error");
      result.hasBlockingErrors = result.hasBlockingErrors || level == "error";
      result.diagnostics.push_back({file, "parse_failed", "incomplete_unit", level,
                                    "The Delphi unit is syntactically incomplete or contains unsupported directives."});
      continue;
    }
    if (!names.insert(lower(ast.name)).second) {
      ++result.statistics.filesParseFailed;
      const auto level = severity("duplicate_unit_source", "error");
      result.hasBlockingErrors = result.hasBlockingErrors || level == "error";
      result.diagnostics.push_back({file, "parse_failed", "duplicate_unit_source", level,
                                    "Another source file declares the same Delphi unit."});
      continue;
    }
    try {
      (void)genericPathToUtf8(file);
      units.push_back(std::move(ast));
    } catch (const std::filesystem::filesystem_error&) {
      const auto level = severity("filesystem_path_encoding_error", "warning");
      result.hasBlockingErrors = result.hasBlockingErrors || level == "error";
      result.diagnostics.push_back({file, "path_encoding_error", "filesystem_path_encoding_error", level,
                                    "The source path could not be represented in UTF-8 and was skipped."});
    }
  }
  result.statistics.unitsIndexed = units.size();
  for (const auto& unit : units) result.statistics.exportsIndexed += unit.exports.size();
  if (units.empty()) {
    const auto level = severity("no_input_units", "error");
    result.hasBlockingErrors = result.hasBlockingErrors || level == "error";
    result.diagnostics.push_back({{}, "index_failed", "no_input_units", level,
                                  "No valid Delphi units found while indexing."});
  }
  std::sort(units.begin(), units.end(), [](const UnitAst& left, const UnitAst& right) {
    const auto leftLower = lower(left.name), rightLower = lower(right.name);
    return leftLower == rightLower ? left.name < right.name : leftLower < rightLower;
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
  output << "# JDelphiAST symbol index v2: unit|symbols|flags|declarations|metadata|inheritance|dependencies\n";
  output << "# statistics files_scanned=" << result.statistics.filesScanned
         << " units_indexed=" << result.statistics.unitsIndexed
         << " files_skipped_non_unit_source=" << result.statistics.filesSkippedNonUnitSource
         << " files_parse_failed=" << result.statistics.filesParseFailed
         << " exports_indexed=" << result.statistics.exportsIndexed << '\n';
  for (const auto& diagnostic : result.diagnostics)
    output << "# source status=" << diagnostic.indexStatus << " code=" << diagnostic.code
           << " file=" << encode(genericPathToUtf8(diagnostic.sourceFile)) << '\n';
  for (const auto& unit : units) {
    output << unit.name << '|';
    std::unordered_set<std::string> parameterNames;
    for (const auto& declaration : unit.declarations)
      for (const auto& parameter : declaration.parameters) parameterNames.insert(lower(parameter.name));
    bool firstSymbol = true;
    for (const auto& symbol : unit.exports) {
      if (parameterNames.contains(lower(symbol.name))) continue;
      const auto classMember = std::any_of(unit.declarations.begin(), unit.declarations.end(), [&](const AstDeclaration& declaration) {
        if (declaration.ownerType.empty()) return false;
        const auto simple = declaration.name.substr(declaration.name.find_last_of('.') + 1);
        return lower(simple) == lower(symbol.name);
      });
      if (classMember) continue;
      if (!firstSymbol) output << ',';
      firstSymbol = false;
      output << symbol.name;
    }
    output << '|';
    bool flag = false;
    if (unit.hasInitialization) { output << "initialization"; flag = true; }
    if (unit.hasFinalization) { if (flag) output << ','; output << "finalization"; }
    output << '|';
    auto declarations = unit.declarations;
    for (const auto& exported : unit.exports) {
      const auto exists = std::any_of(declarations.begin(), declarations.end(), [&](const AstDeclaration& declaration) {
        return lower(declaration.name) == lower(exported.name);
      });
      if (exists || parameterNames.contains(lower(exported.name))) continue;
      AstDeclaration declaration;
      declaration.name = exported.name;
      declaration.kind = exported.kind;
      declaration.visibility = "public";
      declaration.range = exported.range;
      declarations.push_back(std::move(declaration));
    }
    bool firstDeclaration = true;
    for (const auto& declaration : declarations) {
      if (declaration.visibility != "public" && declaration.visibility != "interface") continue;
      if (!firstDeclaration) output << ';';
      firstDeclaration = false;
      std::ostringstream signature;
      signature << declaration.kind << ' ' << declaration.name;
      if (!declaration.parameters.empty()) {
        signature << '(';
        for (std::size_t p = 0; p < declaration.parameters.size(); ++p) {
          if (p) signature << "; ";
          if (!declaration.parameters[p].modifier.empty()) signature << declaration.parameters[p].modifier << ' ';
          signature << declaration.parameters[p].name << ": " << declaration.parameters[p].type;
        }
        signature << ')';
      }
      if (!declaration.type.empty()) signature << ": " << declaration.type;
      output << encode(declaration.kind) << ',' << encode(declaration.name) << ','
             << encode(declaration.type) << ',' << encode(signature.str()) << ','
             << (declaration.overload ? "overload" : "") << ','
             << (declaration.isOverride ? "override" : "");
    }
    output << "|source=" << encode(genericPathToUtf8(unit.file)) << ";version=" << encode(options.version)
           << ";origin=" << encode(options.origin);
    if (const auto metadata = packageByFile.find(lower(pathToUtf8(std::filesystem::absolute(unit.file).lexically_normal())));
        metadata != packageByFile.end()) {
      output << ";package=" << encode(metadata->second.name) << ";dcp=" << encode(metadata->second.dcp)
             << ";bpl=" << encode(metadata->second.bpl) << ";project=" << encode(genericPathToUtf8(metadata->second.project));
    }
    output << '|';
    for (std::size_t relation = 0; relation < unit.inheritance.size(); ++relation) {
      if (relation) output << ';';
      output << encode(unit.inheritance[relation].type) << ',' << encode(unit.inheritance[relation].kind)
             << ',' << encode(unit.inheritance[relation].baseType);
    }
    output << '|';
    for (std::size_t dependency = 0; dependency < unit.uses.size(); ++dependency) {
      if (dependency) output << ',';
      output << encode(unit.uses[dependency].name);
    }
    output << '\n';
  }
  result.content = output.str();
  return result;
}

std::string indexBuildResultJson(const IndexBuildResult& result, const std::filesystem::path& outputPath) {
  const auto escape = [](std::string_view value) {
    std::string escaped;
    for (const char c : value) {
      if (c == '\\') escaped += "\\\\";
      else if (c == '"') escaped += "\\\"";
      else if (c == '\n') escaped += "\\n";
      else if (c == '\r') escaped += "\\r";
      else if (c == '\t') escaped += "\\t";
      else if (static_cast<unsigned char>(c) < 0x20) {
        constexpr char digits[] = "0123456789abcdef";
        escaped += "\\u00";
        escaped += digits[(static_cast<unsigned char>(c) >> 4) & 15];
        escaped += digits[static_cast<unsigned char>(c) & 15];
      } else escaped += c;
    }
    return escaped;
  };
  std::ostringstream output;
  std::size_t errors = 0, warnings = 0, infos = 0;
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.severity == "error") ++errors;
    else if (diagnostic.severity == "warning") ++warnings;
    else ++infos;
  }
  output << "{\"schema_version\":\"1.0\",\"index\":\"" << escape(genericPathToUtf8(outputPath))
         << "\",\"validation\":{\"has_blocking_errors\":" << (result.hasBlockingErrors ? "true" : "false")
         << ",\"error_count\":" << errors << ",\"warning_count\":" << warnings
         << ",\"info_count\":" << infos << ",\"blocking_errors\":[";
  bool firstBlocking = true;
  for (const auto& diagnostic : result.diagnostics) if (diagnostic.severity == "error") {
    if (!firstBlocking) output << ',';
    firstBlocking = false;
    output << "{\"code\":\"" << escape(diagnostic.code) << "\",\"message\":\""
           << escape(diagnostic.message) << "\",\"file\":\"" << escape(genericPathToUtf8(diagnostic.sourceFile)) << "\"}";
  }
  output << "]}"
         << ",\"statistics\":{\"files_scanned\":" << result.statistics.filesScanned
         << ",\"units_indexed\":" << result.statistics.unitsIndexed
         << ",\"files_skipped_non_unit_source\":" << result.statistics.filesSkippedNonUnitSource
         << ",\"files_parse_failed\":" << result.statistics.filesParseFailed
         << ",\"exports_indexed\":" << result.statistics.exportsIndexed << "},\"files\":[";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    if (i) output << ',';
    const auto& diagnostic = result.diagnostics[i];
    output << "{\"source_file\":\"" << escape(genericPathToUtf8(diagnostic.sourceFile))
           << "\",\"index_status\":\"" << diagnostic.indexStatus << "\",\"diagnostics\":[{\"code\":\""
           << diagnostic.code << "\",\"severity\":\"" << diagnostic.severity << "\",\"message\":\""
           << escape(diagnostic.message) << "\"}]}";
  }
  output << "],\"diagnostics\":[";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    if (i) output << ',';
    const auto& diagnostic = result.diagnostics[i];
    output << "{\"code\":\"" << diagnostic.code << "\",\"severity\":\"" << diagnostic.severity
           << "\",\"message\":\"" << escape(diagnostic.message) << "\",\"file\":\""
           << escape(diagnostic.sourceFile.generic_string()) << "\"}";
  }
  output << "],\"blocking_errors\":[";
  bool firstTopLevelError = true;
  for (const auto& diagnostic : result.diagnostics) if (diagnostic.severity == "error") {
    if (!firstTopLevelError) output << ',';
    firstTopLevelError = false;
    output << "{\"code\":\"" << diagnostic.code << "\",\"message\":\""
           << escape(diagnostic.message) << "\",\"file\":\""
           << escape(diagnostic.sourceFile.generic_string()) << "\"}";
  }
  output << "]}";
  return output.str();
}

ProjectLoadResult loadPackage(const std::filesystem::path& packageFile, ProjectOptions options) {
  ProjectLoadResult result;
  result.options = std::move(options);
  const auto package = std::filesystem::absolute(packageFile).lexically_normal();
  if (!std::filesystem::exists(package)) throw std::runtime_error("Package not found: " + pathToUtf8(package));
  const auto packageDir = package.parent_path();
  result.options.searchPaths.insert(result.options.searchPaths.begin(), packageDir);
  const auto dproj = result.options.dprojFile.empty()
      ? package.parent_path() / (pathToUtf8(package.stem()) + ".dproj")
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
  if (!result.options.unitMappingCatalog.empty()) {
    const auto catalog = readFile(result.options.unitMappingCatalog);
    const auto valid = !catalog.empty() && catalog.find("\"schema_version\":\"1.0\"") != std::string::npos &&
                       catalog.find("\"mappings\"") != std::string::npos;
    result.analyzer.setUnitMappingCatalog(result.options.unitMappingCatalog, valid);
  }

  std::unordered_map<std::string, IndexedUnit> mergedIndex;
  for (auto unit : bundledSymbolIndex()) mergedIndex[lower(unit.name)] = std::move(unit);
  for (const auto& index : result.options.indexFiles)
    for (auto unit : loadSymbolIndex(index)) {
      const auto key = lower(unit.name);
      if (auto existing = mergedIndex.find(key); existing != mergedIndex.end()) {
        unit.hasInitialization = unit.hasInitialization || existing->second.hasInitialization;
        unit.hasFinalization = unit.hasFinalization || existing->second.hasFinalization;
      }
      mergedIndex[key] = std::move(unit);
    }
  std::vector<std::string> indexedNames;
  indexedNames.reserve(mergedIndex.size());
  for (const auto& [name, unit] : mergedIndex) indexedNames.push_back(name);
  std::sort(indexedNames.begin(), indexedNames.end());
  for (const auto& name : indexedNames) {
    auto unit = std::move(mergedIndex[name]);
    for (const auto& scope : result.options.namespaces) {
      const auto prefix = scope + ".";
      if (lower(unit.name).starts_with(lower(prefix)))
        result.analyzer.addUnitAlias(unit.name.substr(prefix.size()), unit.name);
    }
    result.analyzer.addIndexedUnit(std::move(unit));
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
    const auto key = lower(pathToUtf8(path));
    if (loaded.contains(key)) continue;
    if (!std::filesystem::exists(path)) {
      result.diagnostics.push_back("Source not found: " + pathToUtf8(path));
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
        if (!loaded.contains(lower(pathToUtf8(std::filesystem::absolute(found->path).lexically_normal()))))
          pending.push_back(found->path);
      }
    }
  }
  for (const auto& diagnostic : result.diagnostics) result.analyzer.addDiagnostic(diagnostic);
  return result;
}

}  // namespace jdelphiast
