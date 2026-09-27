#include "jdelphiast/preprocessor.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
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

bool evaluate(std::string expression, const std::unordered_set<std::string>& defines, bool& known) {
  expression = trim(expression);
  auto upper = canonical(expression);
  bool negate = false;
  if (upper.starts_with("NOT ")) { negate = true; expression = trim(expression.substr(4)); upper = canonical(expression); }
  std::string symbol;
  if (upper.starts_with("DEFINED")) {
    auto rest = trim(expression.substr(7));
    if (!rest.empty() && rest.front() == '(' && rest.back() == ')') rest = trim(rest.substr(1, rest.size() - 2));
    symbol = rest;
  } else {
    known = false;
    return false;
  }
  if (symbol.empty()) { known = false; return false; }
  known = true;
  const auto value = defines.contains(canonical(symbol));
  return negate ? !value : value;
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

  void processInclude(PreprocessResult& target, const std::filesystem::path& includingFile,
                      const std::string& requested, std::size_t depth) {
    const auto found = findInclude(includingFile, requested, options);
    if (!found) { reason(target, "include_not_found: " + requested); return; }
    if (std::find(chain.begin(), chain.end(), *found) != chain.end()) {
      reason(target, "include_cycle: " + found->string());
      return;
    }
    if (depth >= options.maxIncludeDepth) { reason(target, "include_depth_exceeded: " + requested); return; }
    target.includesResolved.push_back(*found);
    std::ifstream input(*found, std::ios::binary);
    if (!input) { reason(target, "include_read_error: " + found->string()); return; }
    const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    chain.push_back(*found);
    auto nested = run(*found, source, depth + 1);
    chain.pop_back();
    target.includesResolved.insert(target.includesResolved.end(), nested.includesResolved.begin(), nested.includesResolved.end());
    target.inactiveRanges.insert(target.inactiveRanges.end(), nested.inactiveRanges.begin(), nested.inactiveRanges.end());
    target.reasons.insert(target.reasons.end(), nested.reasons.begin(), nested.reasons.end());
    target.complete = target.complete && nested.complete;
    defines.clear();
    for (const auto& define : nested.finalDefines) defines.insert(canonical(define));
    if (std::any_of(nested.source.begin(), nested.source.end(), [](unsigned char c) { return !std::isspace(c); }))
      reason(target, "include_contains_code: " + found->string());
  }

  PreprocessResult run(const std::filesystem::path& file, std::string_view source, std::size_t depth) {
    PreprocessResult local;
    local.source = std::string(source);
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
                         : evaluate(argument, defines, known);
          if (!known) { local.complete = false; local.reasons.push_back("unsupported_condition: " + argument); }
          stack.push_back({active, active && known && condition, known && condition, false,
                           command + (argument.empty() ? "" : " " + argument), 0});
          active = stack.back().active;
        } else if (command == "ELSEIF") {
          if (stack.empty() || stack.back().sawElse) { local.complete = false; local.reasons.push_back("unexpected_elseif"); }
          else {
            bool known = true;
            const auto condition = evaluate(argument, defines, known);
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
          processInclude(local, file, argument, depth);
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
