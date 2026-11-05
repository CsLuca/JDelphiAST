#include "jdelphiast/preprocessor.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <unordered_set>

namespace jdelphiast {
namespace {

std::string canonical(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

SourcePosition positionAt(std::string_view source, std::size_t offset) {
  SourcePosition result{offset, 1, 1};
  for (std::size_t i = 0; i < offset && i < source.size(); ++i) {
    if (source[i] == '\n') { ++result.line; result.column = 1; }
    else ++result.column;
  }
  return result;
}

void mask(std::string& output, std::size_t begin, std::size_t end) {
  for (auto i = begin; i < end && i < output.size(); ++i)
    if (output[i] != '\r' && output[i] != '\n') output[i] = ' ';
}

std::filesystem::path portablePath(std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  return value;
}

std::optional<std::filesystem::path> findInclude(const std::filesystem::path& includingFile,
                                                 const std::string& requested,
                                                 const PreprocessorOptions& options) {
  auto relative = portablePath(requested);
  if (relative.is_absolute() && std::filesystem::exists(relative)) return relative;
  const auto local = includingFile.parent_path() / relative;
  if (std::filesystem::exists(local)) return std::filesystem::absolute(local).lexically_normal();
  for (const auto& directory : options.includePaths) {
    const auto candidate = directory / relative;
    if (std::filesystem::exists(candidate)) return std::filesystem::absolute(candidate).lexically_normal();
  }
  return std::nullopt;
}

struct Frame {
  bool parentActive{};
  bool active{};
  bool branchTaken{};
  bool sawElse{};
  std::string condition;
  std::size_t inactiveStart{};
};

bool evaluate(std::string expression, const std::unordered_set<std::string>& defines, bool& known,
              std::optional<double> compilerVersion) {
  expression = trim(expression);
  // Peel only parentheses enclosing the entire expression.
  if (!expression.empty() && expression.front() == '(') {
    int depth = 0;
    bool encloses = false;
    for (std::size_t n = 0; n < expression.size(); ++n) {
      if (expression[n] == '(') ++depth;
      else if (expression[n] == ')') {
        --depth;
        if (depth == 0) { encloses = n == expression.size() - 1; break; }
      }
    }
    if (encloses) return evaluate(expression.substr(1, expression.size() - 2), defines, known, compilerVersion);
  }
  auto upper = canonical(expression);
  // OR has lower precedence than AND; evaluate both operands so unresolved
  // syntax never disappears through Boolean short circuiting.
  for (const auto& op : {std::string("OR"), std::string("AND")}) {
    int depth = 0;
    for (std::size_t n = 0; n < upper.size(); ++n) {
      if (upper[n] == '(') ++depth;
      else if (upper[n] == ')') --depth;
      if (depth < 0) { known = false; return false; }
      if (depth == 0 && upper.substr(n, op.size()) == op &&
          (n == 0 || (!std::isalnum(static_cast<unsigned char>(upper[n - 1])) && upper[n - 1] != '_')) &&
          (n + op.size() == upper.size() || (!std::isalnum(static_cast<unsigned char>(upper[n + op.size()])) && upper[n + op.size()] != '_'))) {
        bool leftKnown = true, rightKnown = true;
        const bool left = evaluate(expression.substr(0, n), defines, leftKnown, compilerVersion);
        const bool right = evaluate(expression.substr(n + op.size()), defines, rightKnown, compilerVersion);
        known = leftKnown && rightKnown;
        return known && (op == "OR" ? left || right : left && right);
      }
    }
    if (depth != 0) { known = false; return false; }
  }
  if (upper.starts_with("NOT ") || upper.starts_with("NOT(")) {
    const bool value = evaluate(expression.substr(3), defines, known, compilerVersion);
    return known && !value;
  }
  std::smatch comparison;
  if (std::regex_match(upper, comparison, std::regex(R"(SIZEOF\((EXTENDED|LONGINT|NATIVEINT|INT64)\)\s*(>=|<=|<>|=|>|<)\s*(?:([0-9]+)|SIZEOF\((EXTENDED|LONGINT|NATIVEINT|INT64)\)))"))) {
    // Explicit Delphi Win32 target only; other targets/types remain unknown.
    known = compilerVersion.has_value() && *compilerVersion == 36.0 && defines.contains("WIN32") && defines.contains("CPUX86") && !defines.contains("WIN64");
    if (!known) return false;
    const auto size = [](const std::string& type) { return type == "EXTENDED" ? 10 : type == "INT64" ? 8 : 4; };
    const auto left = size(comparison[1].str());
    const auto right = comparison[3].matched ? std::stoi(comparison[3].str()) : size(comparison[4].str());
    const auto op = comparison[2].str();
    return op == ">=" ? left >= right : op == "<=" ? left <= right : op == ">" ? left > right :
           op == "<" ? left < right : op == "<>" ? left != right : left == right;
  }
  if (std::regex_match(upper, comparison, std::regex(R"(COMPILERVERSION\s*(>=|<=|<>|=|>|<)\s*([0-9]+(?:\.[0-9]+)?))"))) {
    known = compilerVersion.has_value();
    if (!known) return false;
    const double rhs = std::stod(comparison[2].str());
    const auto op = comparison[1].str();
    return op == ">=" ? *compilerVersion >= rhs : op == "<=" ? *compilerVersion <= rhs :
           op == ">" ? *compilerVersion > rhs : op == "<" ? *compilerVersion < rhs :
           op == "<>" ? *compilerVersion != rhs : *compilerVersion == rhs;
  }
  std::string symbol;
  if (upper.starts_with("DEFINED")) {
    auto rest = trim(expression.substr(7));
    if (!rest.empty() && rest.front() == '(' && rest.back() == ')') rest = trim(rest.substr(1, rest.size() - 2));
    symbol = rest;
  } else {
    known = false;
    return false;
  }
  if (symbol.empty() || !std::regex_match(symbol, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)"))) { known = false; return false; }
  known = true;
  const auto value = defines.contains(canonical(symbol));
  return value;
}

struct Processor {
  const PreprocessorOptions& options;
  std::unordered_set<std::string> defines;
  std::vector<std::filesystem::path> chain;
  void reason(PreprocessResult& target, std::string value) {
    if (std::find(target.reasons.begin(), target.reasons.end(), value) == target.reasons.end())
      target.reasons.push_back(std::move(value));
    target.complete = false;
  }

  PreprocessResult processInclude(PreprocessResult& target, const std::filesystem::path& includingFile,
                      const std::string& requested, std::size_t depth) {
    const auto found = findInclude(includingFile, requested, options);
    if (!found) { reason(target, "include_not_found: " + requested); return {}; }
    if (std::find(chain.begin(), chain.end(), *found) != chain.end()) {
      reason(target, "include_cycle: " + found->string());
      return {};
    }
    if (depth >= options.maxIncludeDepth) { reason(target, "include_depth_exceeded: " + requested); return {}; }
    target.includesResolved.push_back(*found);
    std::ifstream input(*found, std::ios::binary);
    if (!input) { reason(target, "include_read_error: " + found->string()); return {}; }
    const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    chain.push_back(*found);
    auto nested = run(*found, source, depth + 1);
    chain.pop_back();
    target.includesResolved.insert(target.includesResolved.end(), nested.includesResolved.begin(), nested.includesResolved.end());
    target.inactiveRanges.insert(target.inactiveRanges.end(), nested.inactiveRanges.begin(), nested.inactiveRanges.end());
    target.reasons.insert(target.reasons.end(), nested.reasons.begin(), nested.reasons.end());
    target.includedSources.push_back({*found, nested.source});
    target.includedSources.insert(target.includedSources.end(), nested.includedSources.begin(), nested.includedSources.end());
    target.complete = target.complete && nested.complete;
    defines.clear();
    for (const auto& define : nested.finalDefines) defines.insert(canonical(define));
    return nested;
  }

  PreprocessResult run(const std::filesystem::path& file, std::string_view source, std::size_t depth) {
    PreprocessResult local;
    local.source = std::string(source);
    std::vector<std::pair<std::size_t, PreprocessResult>> expansions;
    std::vector<Frame> stack;
    bool active = true;
    bool inString = false;
    bool lineComment = false;
    std::size_t inactiveStart = std::string::npos;
    const auto closeInactive = [&](std::size_t end) {
      if (inactiveStart != std::string::npos && end > inactiveStart) {
        local.inactiveRanges.push_back({file, {positionAt(source, inactiveStart), positionAt(source, end)},
                                        stack.empty() ? "inactive" : stack.back().condition});
        inactiveStart = std::string::npos;
      }
    };
    for (std::size_t i = 0; i < source.size();) {
      if (lineComment) {
        if (source[i] == '\n') lineComment = false;
        if (!active && inactiveStart == std::string::npos) inactiveStart = i;
        if (!active) mask(local.source, i, i + 1);
        ++i;
        continue;
      }
      if (inString) {
        if (source[i] == '\'' && i + 1 < source.size() && source[i + 1] == '\'') { i += 2; continue; }
        if (source[i] == '\'') inString = false;
        if (!active && inactiveStart == std::string::npos) inactiveStart = i;
        if (!active) mask(local.source, i, i + 1);
        ++i;
        continue;
      }
      if (active && source.substr(i, 2) == "//") { lineComment = true; i += 2; continue; }
      if (active && source[i] == '\'') { inString = true; ++i; continue; }
      const bool braceDirective = source.substr(i, 2) == "{$";
      const bool parenDirective = source.substr(i, 3) == "(*$";
      // Ordinary comments can contain directive-looking text, including in
      // inactive branches. Such text must never change the conditional stack.
      if (!braceDirective && !parenDirective && (source[i] == '{' || source.substr(i, 2) == "(*")) {
        const auto terminator = source[i] == '{' ? "}" : "*)";
        const auto close = source.find(terminator, i + (source[i] == '{' ? 1 : 2));
        if (close == std::string_view::npos) { reason(local, "unterminated_comment"); break; }
        const auto end = close + std::string_view(terminator).size();
        if (!active) mask(local.source, i, end);
        i = end;
        continue;
      }
      if (braceDirective || parenDirective) {
        const auto terminator = braceDirective ? "}" : "*)";
        const auto bodyStart = i + (braceDirective ? 2 : 3);
        const auto close = source.find(terminator, bodyStart);
        if (close == std::string_view::npos) { mask(local.source, i, source.size()); local.complete = false; local.reasons.push_back("unterminated_directive"); break; }
        const auto end = close + std::string_view(terminator).size();
        auto body = trim(std::string(source.substr(bodyStart, close - bodyStart)));
        const auto space = body.find_first_of(" \t");
        const auto command = canonical(body.substr(0, space));
        auto argument = space == std::string::npos ? std::string{} : trim(body.substr(space + 1));
        mask(local.source, i, end);
        const auto oldActive = active;
        if (command == "IFDEF" || command == "IFNDEF" || command == "IF") {
          bool known = true;
          bool condition = command == "IFDEF" ? defines.contains(canonical(argument))
                         : command == "IFNDEF" ? !defines.contains(canonical(argument))
                         : evaluate(argument, defines, known, options.compilerVersion);
          if (!known) { local.complete = false; local.reasons.push_back("unsupported_condition: " + argument); }
          stack.push_back({active, active && known && condition, known && condition, false,
                           command + (argument.empty() ? "" : " " + argument), 0});
          active = stack.back().active;
        } else if (command == "ELSEIF") {
          if (stack.empty() || stack.back().sawElse) { local.complete = false; local.reasons.push_back("unexpected_elseif"); }
          else {
            bool known = true;
            const auto condition = evaluate(argument, defines, known, options.compilerVersion);
            if (!known) { local.complete = false; local.reasons.push_back("unsupported_condition: " + argument); }
            auto& frame = stack.back();
            frame.active = frame.parentActive && !frame.branchTaken && known && condition;
            frame.branchTaken = frame.branchTaken || (known && condition);
            frame.condition = "ELSEIF " + argument;
            active = frame.active;
          }
        } else if (command == "ELSE") {
          if (stack.empty() || stack.back().sawElse) { local.complete = false; local.reasons.push_back("unexpected_else"); }
          else {
            auto& frame = stack.back();
            frame.sawElse = true;
            frame.active = frame.parentActive && !frame.branchTaken;
            frame.branchTaken = true;
            frame.condition = "ELSE";
            active = frame.active;
          }
        } else if (command == "ENDIF" || command == "IFEND") {
          if (stack.empty()) { local.complete = false; local.reasons.push_back("unexpected_endif"); }
          else { stack.pop_back(); active = stack.empty() ? true : stack.back().active; }
        } else if (command == "DEFINE" && active) defines.insert(canonical(argument));
        else if (command == "UNDEF" && active) defines.erase(canonical(argument));
        else if ((command == "I" || command == "INCLUDE") && active && !argument.empty() && argument != "+" && argument != "-") {
          if (argument.size() >= 2 && argument.front() == '\'' && argument.back() == '\'') argument = argument.substr(1, argument.size() - 2);
          expansions.emplace_back(end, processInclude(local, file, argument, depth));
        } else if (std::regex_match(command, std::regex(R"([A-Z](?:[+-]|[0-9]+)(?:,[A-Z](?:[+-]|[0-9]+))*)"))) {
          // Preserve the distinction: M controls RTTI/published semantics and
          // remains unresolved; other listed short switches do not select text.
          if (active && std::regex_search(command, std::regex(R"((^|,)[HMJ][+-](,|$))")))
            local.source.replace(i, end - i, source.substr(i, end - i));
        } else if ((command == "TYPEDADDRESS" || command == "WRITEABLECONST" || command == "VARPROPSETTER") && active) {
          const auto setting = canonical(argument);
          if (setting != "ON" && setting != "OFF") reason(local, "invalid_semantic_switch: " + body);
          else local.source.replace(i, end - i, source.substr(i, end - i));
          // These switches change expression/mutation semantics, not which
          // declarations exist. Preserve state for downstream semantic review.
        } else if (command == "SCOPEDENUMS" && active) {
          const auto setting = canonical(argument);
          if (setting != "ON" && setting != "OFF") reason(local, "invalid_scopedenums: " + argument);
          else local.source.replace(i, end - i, source.substr(i, end - i));
          // Preserve this semantic directive in expanded source so the parser
          // sees transitions in source/include order; it is not a branch switch.
        } else if (command == "MINSTACKSIZE" || command == "MAXSTACKSIZE" || command == "IMAGEBASE") {
          // Linker memory layout does not affect Delphi declaration resolution.
        } else if (command == "HPPEMIT" || command == "EXTERNALSYM" || command == "NODEFINE" || command == "NOINCLUDE") {
          // C++ header emission/export annotations do not select Delphi source
          // or change Delphi symbol visibility. Other switches remain explicit
          // unresolved until their semantic effects are represented.
        } else if (command == "REGION" || command == "ENDREGION" || command == "R" || command == "RESOURCE" || command == "WARN" || command == "HINTS" ||
                    command == "WARNINGS" || command == "APPTYPE" || command == "ALIGN" || command == "C" || command == "WEAKPACKAGEUNIT" ||
                   command == "MINENUMSIZE" || command == "R+" || command == "R-" ||
                   command == "Q+" || command == "Q-" || command == "O+" || command == "O-" ||
                   command == "I+" || command == "I-" || command == "T+" || command == "T-") {
          // Compiler/resource switches do not select active source ranges.
        } else if (command != "DEFINE" && command != "UNDEF" && command != "I") {
          local.complete = false;
          local.reasons.push_back("unsupported_directive: " + command);
        }
        if (oldActive && !active) inactiveStart = end;
        if (!oldActive && active) closeInactive(i);
        i = end;
        continue;
      }
      if (!active) {
        if (inactiveStart == std::string::npos) inactiveStart = i;
        mask(local.source, i, i + 1);
      }
      ++i;
    }
    closeInactive(source.size());
    if (!stack.empty()) { local.complete = false; local.reasons.push_back("unterminated_conditional"); }
    local.finalDefines.assign(defines.begin(), defines.end());
    std::sort(local.finalDefines.begin(), local.finalDefines.end());
    SourcePosition original;
    std::size_t expansion = 0;
    auto appendExpansion = [&]() {
      while (expansion < expansions.size() && expansions[expansion].first == original.offset) {
        const auto& included = expansions[expansion++].second;
        local.expandedSource += included.expandedSource;
        if (!included.origins.empty())
          local.origins.insert(local.origins.end(), included.origins.begin(), included.origins.end() - 1);
      }
    };
    for (; original.offset < source.size();) {
      appendExpansion();
      local.origins.push_back({file, original});
      local.expandedSource += local.source[original.offset];
      if (source[original.offset++] == '\n') { ++original.line; original.column = 1; }
      else ++original.column;
    }
    appendExpansion();
    local.origins.push_back({file, original});
    return local;
  }
};

}  // namespace

PreprocessResult preprocess(const std::filesystem::path& file, std::string_view source,
                            const PreprocessorOptions& options) {
  Processor processor{options, {}, {}};
  for (const auto& define : options.defines) processor.defines.insert(canonical(define));
  processor.chain.push_back(std::filesystem::absolute(file).lexically_normal());
  return processor.run(file, source, 0);
}

}  // namespace jdelphiast
