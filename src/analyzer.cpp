#include "jdelphiast/analyzer.hpp"
#include "jdelphiast/preprocessor.hpp"

#include <algorithm>
#include <regex>
#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace jdelphiast {
namespace {

enum class TokenKind { Identifier, String, Symbol, End };

struct Token {
  TokenKind kind;
  std::string text;
  SourceRange range;
};

std::string canonical(std::string_view text) {
  std::string result(text);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

bool isKeyword(std::string_view text) {
  static const std::unordered_set<std::string> words = {
      "absolute", "and", "array", "as", "asm", "begin", "case", "class",
      "const", "constructor", "destructor", "dispinterface", "div", "do", "downto",
      "else", "end", "except", "exports", "file", "finalization", "finally", "for",
      "function", "goto", "if", "implementation", "in", "inherited", "initialization",
      "inline", "interface", "is", "label", "library", "mod", "nil", "not", "object",
      "of", "on", "operator", "or", "out", "packed", "procedure", "program", "property",
      "private", "protected", "public", "published", "raise", "record", "repeat", "resourcestring", "set", "shl", "shr", "string",
      "then", "threadvar", "to", "try", "type", "unit", "until", "uses", "var", "while",
      "with", "xor"};
  return words.contains(canonical(text));
}

bool isBuiltin(std::string_view text) {
  static const std::unordered_set<std::string> words = {
      "ansichar", "ansistring", "boolean", "byte", "cardinal", "char", "comp", "currency",
      "double", "extended", "false", "int64", "integer", "longint", "longword", "nativeint",
      "nativeuint", "pointer", "real", "result", "self", "shortint", "shortstring", "single",
      "smallint", "true", "uint64", "unicodechar", "unicodestring", "variant", "widechar",
      "widestring", "word"};
  return words.contains(canonical(text));
}

class Lexer {
 public:
  explicit Lexer(std::string_view source) : source_(source) {
    // Offsets are original-file byte offsets; BOM is not a Pascal token.
    if (source_.starts_with("\xEF\xBB\xBF")) offset_ = 3;
  }

  std::vector<Token> run(bool* hasDirective = nullptr) {
    std::vector<Token> result;
    while (offset_ < source_.size()) {
      if (std::isspace(static_cast<unsigned char>(source_[offset_]))) {
        advance();
      } else if (starts("//")) {
        skipLineComment();
      } else if (source_[offset_] == '{') {
        if (hasDirective && offset_ + 1 < source_.size() && source_[offset_ + 1] == '$') *hasDirective = true;
        skipComment("}");
      } else if (starts("(*")) {
        if (hasDirective && offset_ + 2 < source_.size() && source_[offset_ + 2] == '$') *hasDirective = true;
        skipComment("*)");
      } else if (source_[offset_] == '\'') {
        result.push_back(readString());
      } else if (isIdentifierStart(source_[offset_])) {
        result.push_back(readIdentifier());
      } else {
        const auto begin = position();
        const char value = source_[offset_];
        advance();
        result.push_back({TokenKind::Symbol, std::string(1, value), {begin, position()}});
      }
    }
    result.push_back({TokenKind::End, {}, {position(), position()}});
    return result;
  }

 private:
  bool starts(std::string_view value) const { return source_.substr(offset_, value.size()) == value; }
  static bool isIdentifierStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
  }
  static bool isIdentifierPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  }
  SourcePosition position() const { return {offset_, line_, column_}; }
  void advance() {
    if (source_[offset_++] == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
  }
  void skipLineComment() {
    while (offset_ < source_.size() && source_[offset_] != '\n') advance();
  }
  void skipComment(std::string_view terminator) {
    advance();
    if (terminator.size() == 2) advance();
    while (offset_ < source_.size() && !starts(terminator)) advance();
    for (std::size_t i = 0; i < terminator.size() && offset_ < source_.size(); ++i) advance();
  }
  Token readIdentifier() {
    const auto begin = position();
    const auto start = offset_;
    while (offset_ < source_.size() && isIdentifierPart(source_[offset_])) advance();
    return {TokenKind::Identifier, std::string(source_.substr(start, offset_ - start)), {begin, position()}};
  }
  Token readString() {
    const auto begin = position();
    const auto start = offset_;
    std::size_t quoteCount = 0;
    while (start + quoteCount < source_.size() && source_[start + quoteCount] == '\'') ++quoteCount;
    if (quoteCount >= 3 && quoteCount % 2 == 1 && start + quoteCount < source_.size() &&
        (source_[start + quoteCount] == '\r' || source_[start + quoteCount] == '\n')) {
      for (std::size_t n = 0; n < quoteCount; ++n) advance();
      const auto closing = source_.find(std::string(quoteCount, '\''), offset_);
      const auto end = closing == std::string_view::npos ? source_.size() : closing + quoteCount;
      while (offset_ < end) advance();
      return {TokenKind::String, std::string(source_.substr(start, offset_ - start)), {begin, position()}};
    }
    advance();
    while (offset_ < source_.size()) {
      if (source_[offset_] != '\'') {
        advance();
      } else {
        advance();
        if (offset_ < source_.size() && source_[offset_] == '\'') advance();
        else break;
      }
    }
    return {TokenKind::String, std::string(source_.substr(start, offset_ - start)), {begin, position()}};
  }

  std::string_view source_;
  std::size_t offset_{};
  std::size_t line_{1};
  std::size_t column_{1};
};

bool word(const Token& token, std::string_view expected) {
  return token.kind == TokenKind::Identifier && canonical(token.text) == expected;
}

std::string qualifiedName(const std::vector<Token>& tokens, std::size_t& index, SourceRange* range = nullptr) {
  if (tokens[index].kind != TokenKind::Identifier) return {};
  std::string result = tokens[index].text;
  SourceRange actual = tokens[index].range;
  ++index;
  while (tokens[index].text == "." && tokens[index + 1].kind == TokenKind::Identifier) {
    result += "." + tokens[index + 1].text;
    actual.end = tokens[index + 1].range.end;
    index += 2;
  }
  if (range) *range = actual;
  return result;
}

std::string sourceSlice(const std::string& source, SourceRange range) {
  return source.substr(range.begin.offset, range.end.offset - range.begin.offset);
}

std::string argumentKind(const Token& token) {
  if (token.kind == TokenKind::String) return "string";
  if (word(token, "nil")) return "nil";
  if (word(token, "true") || word(token, "false")) return "boolean";
  return token.kind == TokenKind::Identifier ? "identifier" : "expression";
}

std::string builtinType(const Token& token) {
  if (token.kind == TokenKind::String) return "string";
  if (word(token, "true") || word(token, "false")) return "Boolean";
  return {};
}

std::string sha256(std::string_view input) {
  constexpr std::array<std::uint32_t, 64> constants = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::vector<std::uint8_t> data(input.begin(), input.end());
  const auto bitLength = static_cast<std::uint64_t>(data.size()) * 8;
  data.push_back(0x80);
  while (data.size() % 64 != 56) data.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8) data.push_back(static_cast<std::uint8_t>(bitLength >> shift));
  std::array<std::uint32_t, 8> hash = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const auto rotate = [](std::uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); };
  for (std::size_t block = 0; block < data.size(); block += 64) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t n = 0; n < 16; ++n) {
      const auto at = block + n * 4;
      words[n] = (static_cast<std::uint32_t>(data[at]) << 24) |
                 (static_cast<std::uint32_t>(data[at + 1]) << 16) |
                 (static_cast<std::uint32_t>(data[at + 2]) << 8) | data[at + 3];
    }
    for (std::size_t n = 16; n < 64; ++n) {
      const auto s0 = rotate(words[n - 15], 7) ^ rotate(words[n - 15], 18) ^ (words[n - 15] >> 3);
      const auto s1 = rotate(words[n - 2], 17) ^ rotate(words[n - 2], 19) ^ (words[n - 2] >> 10);
      words[n] = words[n - 16] + s0 + words[n - 7] + s1;
    }
    auto a = hash[0], b = hash[1], c = hash[2], d = hash[3];
    auto e = hash[4], f = hash[5], g = hash[6], h = hash[7];
    for (std::size_t n = 0; n < 64; ++n) {
      const auto sum1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
      const auto choice = (e & f) ^ (~e & g);
      const auto temp1 = h + sum1 + choice + constants[n] + words[n];
      const auto sum0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = sum0 + majority;
      h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
    }
    hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
    hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
  }
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (const auto value : hash) output << std::setw(8) << value;
  return output.str();
}

UnitAst parseSource(const std::filesystem::path& path, const std::string& source) {
  bool hasDirective = false;
  const auto tokens = Lexer(source).run(&hasDirective);
  UnitAst ast;
  ast.file = path;
  ast.sourceHash = sha256(source);
  ast.range = {{0, 1, 1}, tokens.back().range.end};
  ast.complete = !hasDirective;
  const auto concrete = tokenizeConcrete(source);
  std::vector<std::pair<std::size_t, bool>> scopedEnumStates;
  struct SwitchState { std::size_t offset; char name; bool enabled; };
  std::vector<SwitchState> semanticSwitches;
  bool onlyScopedDirectives = true;
  for (const auto& token : concrete.tokens) {
    if (token.kind != "directive") continue;
    auto setting = canonical(token.bytes);
    const bool scoped = setting.find("scopedenums") != std::string::npos;
    const bool on = setting.find(" on") != std::string::npos;
    const bool off = setting.find(" off") != std::string::npos;
    if (scoped && (on || off)) scopedEnumStates.emplace_back(token.range.begin.offset, on);
    else {
      auto body = setting;
      if (body.starts_with("{$")) body = body.substr(2, body.size() - 3);
      else if (body.starts_with("(*$")) body = body.substr(3, body.size() - 5);
      body.erase(std::remove_if(body.begin(), body.end(), [](unsigned char c) { return std::isspace(c); }), body.end());
      const auto namedState = [&](const std::string& directive, char key) {
        if (body == directive + "on" || body == directive + "off") {
          semanticSwitches.push_back({token.range.begin.offset, key, body == directive + "on"});
          return true;
        }
        return false;
      };
      if (namedState("typedaddress", 't') || namedState("writeableconst", 'j') || namedState("varpropsetter", 'v')) continue;
      bool supported = !body.empty();
      std::size_t start = 0;
      while (start < body.size()) {
        auto end = body.find(',', start);
        if (end == std::string::npos) end = body.size();
        const auto part = body.substr(start, end - start);
        if (part.size() == 2 && (part[1] == '+' || part[1] == '-') &&
            (part[0] == 'h' || part[0] == 'm' || part[0] == 'j'))
          semanticSwitches.push_back({token.range.begin.offset, part[0], part[1] == '+'});
        else if (part.size() < 2 || !std::isalpha(static_cast<unsigned char>(part[0])) ||
                 !(part[1] == '+' || part[1] == '-' || std::isdigit(static_cast<unsigned char>(part[1])))) supported = false;
        start = end + 1;
      }
      if (!supported) onlyScopedDirectives = false;
    }
  }
  if (hasDirective && onlyScopedDirectives) ast.complete = true;
  if (!concrete.complete) {
    ast.complete = false;
    ast.preprocessorReasons.insert(ast.preprocessorReasons.end(), concrete.diagnostics.begin(), concrete.diagnostics.end());
  }
  std::size_t headerCursor = !tokens.empty() && word(tokens[0], "unit") ? 1 : 0;
  if (headerCursor) ast.name = qualifiedName(tokens, headerCursor);
  std::vector<std::string> checkedDelimiters;
  for (const auto& token : tokens) {
    if (token.text == "(" || token.text == "[") checkedDelimiters.push_back(token.text);
    else if (token.text == ")" || token.text == "]") {
      if (checkedDelimiters.empty() || (token.text == ")" ? checkedDelimiters.back() != "(" : checkedDelimiters.back() != "[")) {
        ast.complete = false;
        ast.parseDiagnostics.push_back({"syntax_error", "Unexpected closing delimiter", token.range.begin});
        return ast;
      }
      checkedDelimiters.pop_back();
    }
  }
  if (!checkedDelimiters.empty()) {
    ast.complete = false;
    const auto opening = std::find_if(tokens.begin(), tokens.end(), [](const Token& token) { return token.text == "(" || token.text == "["; });
    ast.parseDiagnostics.push_back({"incomplete_unit", "Unclosed delimiter; partial interface only", opening->range.begin});
    // Recover only named interface headers preceding the malformed delimiter.
    for (std::size_t n = headerCursor; n + 1 < tokens.size(); ++n) {
      if (word(tokens[n], "implementation")) break;
      if ((word(tokens[n], "procedure") || word(tokens[n], "function")) &&
          tokens[n + 1].kind == TokenKind::Identifier && !isKeyword(tokens[n + 1].text))
        ast.exports.push_back({tokens[n + 1].text, canonical(tokens[n].text), tokens[n + 1].range});
    }
    return ast;
  }
  std::size_t i = 0;
  std::unordered_set<std::size_t> typeBodyTokens;
  std::unordered_map<std::size_t, std::string> typeOwners;
  // Parse type headers independently of parameter modifiers (const/var).
  for (std::size_t t = 1; t + 3 < tokens.size(); ++t) {
    if (tokens[t].kind != TokenKind::Identifier || isKeyword(tokens[t].text)) continue;
    std::size_t equal = t + 1;
    if (tokens[equal].text == "<") {
      int genericDepth = 1;
      while (genericDepth && ++equal < tokens.size()) {
        if (tokens[equal].text == "<") ++genericDepth;
        if (tokens[equal].text == ">") --genericDepth;
      }
      ++equal;
    }
    if (equal + 2 >= tokens.size() || tokens[equal].text != "=" ||
        !(word(tokens[equal + 1], "class") || word(tokens[equal + 1], "record") ||
          word(tokens[equal + 1], "interface"))) continue;
    AstDeclaration header;
    header.name = tokens[t].text;
    header.kind = canonical(tokens[equal + 1].text);
    header.visibility = "public";
    header.range = {tokens[t].range.begin, tokens[equal + 1].range.end};
    std::size_t body = equal + 2;
    if (word(tokens[equal + 1], "class") && word(tokens[body], "of")) {
      header.kind = "type";
      header.type = tokens[body + 1].text;
      header.signature = sourceSlice(source, {tokens[t].range.begin, tokens[body + 1].range.end});
      ast.declarations.push_back(header);
      continue; // metaclass reference has no class body
    }
    if (word(tokens[body], "helper")) {
      header.kind += "Helper";
      while (body < tokens.size() && !word(tokens[body], "for") && tokens[body].text != ";") ++body;
      if (body + 1 < tokens.size() && word(tokens[body], "for")) {
        header.type = tokens[body + 1].text;
        body += 2;
      }
    }
    header.signature = sourceSlice(source, {tokens[t].range.begin, tokens[body].range.begin});
    std::size_t afterBase = body;
    if (tokens[afterBase].text == "(") {
      int baseDepth = 1;
      while (tokens[++afterBase].kind != TokenKind::End && baseDepth) {
        if (tokens[afterBase].text == "(") ++baseDepth;
        else if (tokens[afterBase].text == ")") --baseDepth;
      }
    }
    const bool noBody = tokens[body].text == ";" || tokens[afterBase].text == ";";
    header.forwardDeclaration = tokens[body].text == ";";
    ast.declarations.push_back(header);
    if (noBody) continue; // forward or empty ancestor-only class declaration
    int depth = 1;
    for (std::size_t n = body; n < tokens.size() && tokens[n].kind != TokenKind::End; ++n) {
      typeBodyTokens.insert(n);
      typeOwners[n] = header.name;
      if (word(tokens[n], "end")) {
        if (--depth == 0) break;
      } else if ((word(tokens[n], "class") || word(tokens[n], "record")) &&
                 n > 0 && tokens[n - 1].text == "=") ++depth;
    }
  }
  if (!word(tokens[i], "unit")) {
    ast.complete = false;
    return ast;
  }
  ++i;
  ast.name = qualifiedName(tokens, i);
  if (ast.name.empty()) ast.complete = false;

  UsesSection section = UsesSection::Interface;
  bool inInterface = false;
  bool inImplementation = false;
  bool sawInterface = false;
  bool sawImplementation = false;
  bool inInitialization = false;
  std::unordered_set<std::size_t> excluded;
  std::unordered_set<std::string> ownNames;
  std::unordered_set<std::string> lexicalNames;
  std::unordered_map<std::string, std::string> variableTypes;
  std::string currentRoutine;
  std::unordered_set<std::size_t> routineHeaderTokens;
  for (std::size_t r = 0; tokens[r].kind != TokenKind::End; ++r) {
    if (!(word(tokens[r], "procedure") || word(tokens[r], "function") ||
          word(tokens[r], "constructor") || word(tokens[r], "destructor"))) continue;
    int parentheses = 0;
    for (std::size_t n = r; tokens[n].kind != TokenKind::End; ++n) {
      routineHeaderTokens.insert(n);
      if (tokens[n].text == "(") ++parentheses;
      else if (tokens[n].text == ")") --parentheses;
      else if (tokens[n].text == ";" && parentheses == 0) break;
    }
  }

  for (i = 0; tokens[i].kind != TokenKind::End; ++i) {
    if (word(tokens[i], "interface") && (i == 0 || tokens[i - 1].text != "=")) {
      sawInterface = true;
      inInterface = true;
      inImplementation = false;
      section = UsesSection::Interface;
      continue;
    }
    if (word(tokens[i], "implementation")) {
      sawImplementation = true;
      inInterface = false;
      inImplementation = true;
      section = UsesSection::Implementation;
      inInitialization = false;
      continue;
    }
    if (word(tokens[i], "initialization")) {
      ast.hasInitialization = true;
      inImplementation = true;
      inInitialization = true;
      continue;
    }
    if (inInitialization && tokens[i].kind == TokenKind::Identifier && tokens[i + 1].text == "(") {
      const auto call = canonical(tokens[i].text);
      const auto registration = call.starts_with("register") || call == "regcsproc" ||
                                call.starts_with("addfactory") || call.starts_with("install");
      if (registration) {
        const auto argument = tokens[i + 2].kind == TokenKind::Identifier ? tokens[i + 2].text : std::string{};
        if (call == "registerclass") ast.sideEffectReasons.push_back("class_registration: " + argument);
        else if (call.find("factory") != std::string::npos)
          ast.sideEffectReasons.push_back("factory_registration: " + argument);
        else ast.sideEffectReasons.push_back("registration_call: " + tokens[i].text +
                                             (argument.empty() ? "" : ": " + argument));
      }
    }
    if (word(tokens[i], "finalization")) {
      ast.hasFinalization = true;
      inImplementation = true;
      inInitialization = false;
      continue;
    }
    if (word(tokens[i], "uses") && (inInterface || inImplementation)) {
      excluded.insert(i);
      ++i;
      while (tokens[i].kind != TokenKind::End && tokens[i].text != ";") {
        if (tokens[i].kind == TokenKind::Identifier) {
          const auto first = i;
          SourceRange range;
          auto name = qualifiedName(tokens, i, &range);
          ast.uses.push_back({std::move(name), section, range});
          for (auto n = first; n < i; ++n) excluded.insert(n);
          if (word(tokens[i], "in") && tokens[i + 1].kind == TokenKind::String) i += 2;
          if (tokens[i].text == ",") ++i;
        } else {
          ++i;
        }
      }
      if (tokens[i].kind == TokenKind::End) {
        ast.complete = false;
        break;
      }
      excluded.insert(i);
      continue;
    }

    if (inInterface && !typeBodyTokens.contains(i) && (word(tokens[i], "procedure") || word(tokens[i], "function") ||
                        word(tokens[i], "constructor") || word(tokens[i], "destructor"))) {
      if (tokens[i + 1].kind == TokenKind::Identifier && !isKeyword(tokens[i + 1].text)) {
        ast.exports.push_back({tokens[i + 1].text, canonical(tokens[i].text), tokens[i + 1].range});
        ownNames.insert(canonical(tokens[i + 1].text));
        excluded.insert(i + 1);
      }
    }
    if (!typeBodyTokens.contains(i) && (word(tokens[i], "procedure") || word(tokens[i], "function") || word(tokens[i], "constructor") ||
        word(tokens[i], "destructor"))) {
      std::size_t n = i + 1;
      SourceRange nameRange;
      const auto name = isKeyword(tokens[n].text) ? std::string{} : qualifiedName(tokens, n, &nameRange);
      if (!name.empty()) {
        AstDeclaration declaration;
        declaration.kind = canonical(tokens[i].text);
        declaration.name = name;
        declaration.visibility = inInterface ? "public" : "private";
        declaration.scope = ast.name;
        declaration.range = {tokens[i].range.begin, nameRange.end};
        currentRoutine = name;
        if (n < tokens.size() && tokens[n].text == "(") {
          const auto open = n++;
          while (n < tokens.size() && tokens[n].kind != TokenKind::End && tokens[n].text != ")") {
            if (tokens[n].text == ":" && n + 1 < tokens.size() && tokens[n + 1].kind == TokenKind::Identifier) {
              std::size_t typeIndex = n + 1;
              SourceRange typeRange;
              auto parameterType = qualifiedName(tokens, typeIndex, &typeRange);
              if (word(tokens[n + 1], "array") && word(tokens[typeIndex], "of")) {
                ++typeIndex;
                if (tokens[typeIndex].kind == TokenKind::Identifier) {
                  parameterType += " of " + tokens[typeIndex].text;
                  typeRange.end = tokens[typeIndex++].range.end;
                }
              }
              std::size_t defaultEnd = typeIndex;
              std::optional<std::string> defaultValue;
              if (tokens[defaultEnd].text == "=") {
                const auto first = ++defaultEnd;
                int depth = 0;
                while (tokens[defaultEnd].kind != TokenKind::End) {
                  const auto& value = tokens[defaultEnd].text;
                  if (!depth && (value == ";" || value == ")")) break;
                  if (value == "(" || value == "[") ++depth;
                  else if (value == ")" || value == "]") --depth;
                  ++defaultEnd;
                }
                if (defaultEnd > first) defaultValue = sourceSlice(source, {tokens[first].range.begin, tokens[defaultEnd - 1].range.end});
              }
              std::size_t groupStart = n;
              while (groupStart > open + 1 && tokens[groupStart - 1].text != ";") --groupStart;
              std::string modifier;
              if (groupStart < n && (word(tokens[groupStart], "const") || word(tokens[groupStart], "var") ||
                                     word(tokens[groupStart], "out"))) {
                modifier = canonical(tokens[groupStart].text);
                ++groupStart;
              }
              for (std::size_t parameterIndex = groupStart; parameterIndex < n; ++parameterIndex) {
                if (tokens[parameterIndex].kind != TokenKind::Identifier || isKeyword(tokens[parameterIndex].text)) continue;
                AstParameter parameter{tokens[parameterIndex].text, parameterType,
                                       {tokens[parameterIndex].range.begin, typeRange.end}, modifier};
                declaration.parameters.push_back(parameter);
                declaration.parameters.back().defaultValue = defaultValue;
                variableTypes[canonical(parameter.name)] = parameter.type;
                lexicalNames.insert(canonical(parameter.name));
              }
              n = defaultEnd - 1;
            }
            ++n;
          }
          if (n < tokens.size() && tokens[n].text == ")") declaration.range.end = tokens[n].range.end;
          else ast.complete = false;
          n = std::max(n, open + 1);
        }
        const auto returnColon = n < tokens.size() && tokens[n].text == ":" ? n : n + 1;
        if (word(tokens[i], "function") && returnColon + 1 < tokens.size() && tokens[returnColon].text == ":" &&
            tokens[returnColon + 1].kind == TokenKind::Identifier)
          declaration.type = tokens[returnColon + 1].text;
        std::size_t directive = n;
        while (tokens[directive].kind != TokenKind::End && directive < n + 20 &&
               !word(tokens[directive], "begin") && !word(tokens[directive], "implementation")) {
          if (word(tokens[directive], "overload")) declaration.overload = true;
          if (word(tokens[directive], "override")) declaration.isOverride = true;
          if (word(tokens[directive], "reintroduce")) declaration.reintroduced = true;
          if (word(tokens[directive], "deprecated")) declaration.deprecated = true;
          ++directive;
        }
        ast.declarations.push_back(std::move(declaration));
      }
    }
    if (inInterface && word(tokens[i], "type")) {
      std::size_t n = i + 1;
      while (tokens[n].kind != TokenKind::End && !word(tokens[n], "implementation") &&
             (typeBodyTokens.contains(n) || routineHeaderTokens.contains(n) ||
              (!word(tokens[n], "const") && !word(tokens[n], "var") && !word(tokens[n], "uses")))) {
        if (!typeBodyTokens.contains(n) && !routineHeaderTokens.contains(n) &&
            tokens[n].kind == TokenKind::Identifier && tokens[n + 1].text == "=") {
          std::string kind = "type";
          if (word(tokens[n + 2], "class") && !word(tokens[n + 3], "of")) kind = "class";
          else if (word(tokens[n + 2], "interface")) kind = "interface";
          else if (word(tokens[n + 2], "record")) kind = "record";
          else if (tokens[n + 2].text == "(") kind = "enum";
          ast.exports.push_back({tokens[n].text, kind, tokens[n].range});
          if (kind == "enum") {
            bool scoped = false;
            for (const auto& state : scopedEnumStates)
              if (state.first < tokens[n].range.begin.offset) scoped = state.second;
            bool expectValue = true;
            for (std::size_t value = n + 3; tokens[value].kind != TokenKind::End && tokens[value].text != ")"; ++value) {
              if (tokens[value].text == ",") { expectValue = true; continue; }
              if (!expectValue || tokens[value].kind != TokenKind::Identifier || isKeyword(tokens[value].text)) continue;
              AstDeclaration enumerator;
              enumerator.name = tokens[n].text + "." + tokens[value].text;
              enumerator.kind = "enumValue";
              enumerator.type = tokens[n].text;
              enumerator.visibility = "public";
              enumerator.range = tokens[value].range;
              ast.declarations.push_back(enumerator);
              if (!scoped) ast.exports.push_back({tokens[value].text, "enumValue", tokens[value].range});
              expectValue = false;
            }
          }
          ownNames.insert(canonical(tokens[n].text));
          excluded.insert(n);
        }
        ++n;
      }
    }
    if (inInterface && !typeBodyTokens.contains(i) && !routineHeaderTokens.contains(i) &&
        (word(tokens[i], "const") || word(tokens[i], "resourcestring")) &&
        (i == 0 || tokens[i - 1].text == ";" || word(tokens[i - 1], "interface"))) {
      std::size_t n = i + 1;
      while (tokens[n].kind != TokenKind::End && !word(tokens[n], "implementation") &&
             !word(tokens[n], "type") && !word(tokens[n], "var") && !word(tokens[n], "uses") &&
             !word(tokens[n], "procedure") && !word(tokens[n], "function")) {
        if (tokens[n].kind == TokenKind::Identifier &&
            (tokens[n + 1].text == "=" || tokens[n + 1].text == ":")) {
          ast.exports.push_back({tokens[n].text, "const", tokens[n].range});
          ownNames.insert(canonical(tokens[n].text));
          excluded.insert(n);
        }
        ++n;
      }
    }
    if (inInterface && !typeBodyTokens.contains(i) && !routineHeaderTokens.contains(i) &&
        (word(tokens[i], "var") || word(tokens[i], "threadvar"))) {
      std::size_t n = i + 1;
      while (tokens[n].kind != TokenKind::End && !word(tokens[n], "implementation") &&
             !word(tokens[n], "type") && !word(tokens[n], "const") && !word(tokens[n], "uses") &&
             !word(tokens[n], "procedure") && !word(tokens[n], "function")) {
        if (tokens[n].kind == TokenKind::Identifier && tokens[n + 1].text == ":" &&
            tokens[n + 2].kind == TokenKind::Identifier) {
          ast.exports.push_back({tokens[n].text, "variable", tokens[n].range});
          ownNames.insert(canonical(tokens[n].text));
          excluded.insert(n);
          std::size_t typeEnd = n + 2;
          SourceRange typeRange;
          const auto type = qualifiedName(tokens, typeEnd, &typeRange);
          AstDeclaration declaration;
          declaration.kind = "variable";
          declaration.name = tokens[n].text;
          declaration.visibility = "public";
          declaration.type = type;
          declaration.scope = ast.name;
          declaration.range = {tokens[n].range.begin, typeRange.end};
          ast.declarations.push_back(std::move(declaration));
          variableTypes[canonical(tokens[n].text)] = type;
        }
        ++n;
      }
    }
  }

  // Exclude declaration sites. Full lexical shadowing is intentionally deferred; retaining a
  // doubtful reference is safer than recommending removal of a real dependency.
  for (i = 0; tokens[i].kind != TokenKind::End; ++i) {
    if ((word(tokens[i], "procedure") || word(tokens[i], "function") || word(tokens[i], "constructor") ||
         word(tokens[i], "destructor")) && tokens[i + 1].kind == TokenKind::Identifier) {
      excluded.insert(i + 1);
      std::size_t declarationEnd = i + 1;
      qualifiedName(tokens, declarationEnd);
      for (std::size_t part = i + 1; part < declarationEnd; ++part) excluded.insert(part);
    }
    if (tokens[i].kind == TokenKind::Identifier && tokens[i + 1].text == ":" &&
        (i == 0 || !word(tokens[i - 1], "on"))) {
      excluded.insert(i);
      lexicalNames.insert(canonical(tokens[i].text));
    }
    if (word(tokens[i], "on") && tokens[i + 1].kind == TokenKind::Identifier && tokens[i + 2].text == ":") {
      excluded.insert(i + 1);
      lexicalNames.insert(canonical(tokens[i + 1].text));
    }
  }

  bool hasTerminatingEnd = false;
  if (tokens.size() >= 3) {
    const auto endIndex = tokens.size() - 3;
    hasTerminatingEnd = word(tokens[endIndex], "end") && tokens[endIndex + 1].text == ".";
    if (hasTerminatingEnd && sawImplementation) {
      int depth = 0;
      for (std::size_t n = endIndex; n-- > 0;) {
        if (word(tokens[n], "end")) ++depth;
        else if (word(tokens[n], "begin")) {
          if (depth == 0) {
            ast.hasInitialization = true;
            break;
          }
          --depth;
        }
        if (word(tokens[n], "implementation") && depth == 0) break;
      }
    }
  }
  ast.complete = ast.complete && sawInterface && sawImplementation && hasTerminatingEnd;
  if (!sawInterface || !sawImplementation || !hasTerminatingEnd)
    ast.parseDiagnostics.push_back({"incomplete_unit", "Missing interface, implementation or final end.", tokens.back().range.end});
  // Reject malformed delimiter streams before subsequent look-ahead passes.
  std::vector<std::string> delimiters;
  for (const auto& token : tokens) {
    if (token.text == "(" || token.text == "[") delimiters.push_back(token.text);
    else if (token.text == ")" || token.text == "]") {
      if (delimiters.empty() || (token.text == ")" ? delimiters.back() != "(" : delimiters.back() != "[")) {
        ast.complete = false;
        return ast;
      }
      delimiters.pop_back();
    }
  }
  if (!delimiters.empty()) { ast.complete = false; return ast; }

  struct LexicalScope {
    std::size_t begin{};
    std::size_t end{};
    std::unordered_set<std::string> names;
  };
  std::vector<LexicalScope> lexicalScopes;
  bool implementationScope = false;
  for (std::size_t routine = 0; tokens[routine].kind != TokenKind::End; ++routine) {
    if (word(tokens[routine], "implementation")) { implementationScope = true; continue; }
    if (!implementationScope || !(word(tokens[routine], "procedure") || word(tokens[routine], "function") ||
                                  word(tokens[routine], "constructor") || word(tokens[routine], "destructor"))) continue;
    std::size_t end = routine + 1;
    while (tokens[end].kind != TokenKind::End && !word(tokens[end], "initialization") &&
           !word(tokens[end], "finalization") &&
           !(end > routine + 1 && (word(tokens[end], "procedure") || word(tokens[end], "function") ||
                                   word(tokens[end], "constructor") || word(tokens[end], "destructor")))) ++end;
    LexicalScope scope{tokens[routine].range.begin.offset, tokens[end].range.begin.offset, {}};
    std::size_t headerEnd = routine;
    while (tokens[headerEnd].kind != TokenKind::End && tokens[headerEnd].text != ";") ++headerEnd;
    for (std::size_t n = routine + 1; n < headerEnd; ++n) {
      if (tokens[n].text != ":") continue;
      std::size_t first = n;
      while (first > routine + 1 && (tokens[first - 1].kind == TokenKind::Identifier || tokens[first - 1].text == ",")) --first;
      for (auto name = first; name < n; ++name)
        if (tokens[name].kind == TokenKind::Identifier && !isKeyword(tokens[name].text))
          scope.names.insert(canonical(tokens[name].text));
    }
    bool inVars = false;
    for (std::size_t n = headerEnd + 1; n < end; ++n) {
      if (word(tokens[n], "var") || word(tokens[n], "threadvar")) { inVars = true; continue; }
      if (word(tokens[n], "begin")) inVars = false;
      if (inVars && tokens[n].text == ":") {
        std::size_t first = n;
        while (first > headerEnd + 1 && (tokens[first - 1].kind == TokenKind::Identifier || tokens[first - 1].text == ",")) --first;
        for (auto name = first; name < n; ++name)
          if (tokens[name].kind == TokenKind::Identifier && !isKeyword(tokens[name].text))
            scope.names.insert(canonical(tokens[name].text));
      }
      if (word(tokens[n], "on") && tokens[n + 1].kind == TokenKind::Identifier && tokens[n + 2].text == ":")
        scope.names.insert(canonical(tokens[n + 1].text));
    }
    lexicalScopes.push_back(std::move(scope));
  }

  for (i = 0; tokens[i].kind != TokenKind::End; ++i) {
    const auto scope = std::find_if(lexicalScopes.begin(), lexicalScopes.end(), [&](const LexicalScope& candidate) {
      return tokens[i].range.begin.offset >= candidate.begin && tokens[i].range.begin.offset < candidate.end;
    });
    if (scope == lexicalScopes.end() || !scope->names.contains(canonical(tokens[i].text))) continue;
    excluded.insert(i);
    std::size_t n = i + 1;
    while (tokens[n].text == "." && tokens[n + 1].kind == TokenKind::Identifier) {
      excluded.insert(n + 1);
      n += 2;
    }
  }

  section = UsesSection::Interface;
  for (i = 0; tokens[i].kind != TokenKind::End; ++i) {
    if (word(tokens[i], "implementation")) section = UsesSection::Implementation;
    if (tokens[i].kind != TokenKind::Identifier || excluded.contains(i) || isKeyword(tokens[i].text)) continue;
    if (ownNames.contains(canonical(tokens[i].text))) continue;
    // Scope-aware suppression is intentionally deferred; global suppression could hide a
    // same-named imported symbol used by another routine and create an unsafe removal.
    std::size_t end = i + 1;
    std::string name = tokens[i].text;
    SourceRange range = tokens[i].range;
    while (tokens[end].text == "." && tokens[end + 1].kind == TokenKind::Identifier) {
      name += "." + tokens[end + 1].text;
      range.end = tokens[end + 1].range.end;
      end += 2;
    }
    ast.references.push_back({std::move(name), range, section, ResolutionStatus::Unresolved, {}, {}, {}, {}, {}, {}, {}, {}});
    i = end - 1;
  }


  currentRoutine.clear();
  bool localVarSection = false;
  for (i = 0; tokens[i].kind != TokenKind::End; ++i) {
    if ((word(tokens[i], "procedure") || word(tokens[i], "function") || word(tokens[i], "constructor") ||
         word(tokens[i], "destructor")) && tokens[i + 1].kind == TokenKind::Identifier) {
      std::size_t n = i + 1;
      currentRoutine = qualifiedName(tokens, n);
      localVarSection = false;
      continue;
    }
    if (!currentRoutine.empty() && word(tokens[i], "var")) { localVarSection = true; continue; }
    if (localVarSection && word(tokens[i], "begin")) localVarSection = false;
    if (localVarSection && i + 2 < tokens.size() && tokens[i].kind == TokenKind::Identifier && tokens[i + 1].text == ":" &&
        tokens[i + 2].kind == TokenKind::Identifier) {
      std::size_t typeEnd = i + 2;
      SourceRange typeRange;
      const auto type = qualifiedName(tokens, typeEnd, &typeRange);
      AstDeclaration declaration;
      declaration.kind = "localVariable";
      declaration.name = tokens[i].text;
      declaration.visibility = "private";
      declaration.type = type;
      declaration.scope = currentRoutine;
      declaration.range = {tokens[i].range.begin, typeRange.end};
      ast.declarations.push_back(std::move(declaration));
      variableTypes[canonical(tokens[i].text)] = type;
    }
  }

  for (i = 0; tokens[i].kind != TokenKind::End; ++i) {
    if (tokens[i].kind != TokenKind::Identifier || tokens[i + 1].text != "=" ||
        !(word(tokens[i + 2], "class") || word(tokens[i + 2], "interface") || word(tokens[i + 2], "record"))) continue;
    InheritanceRelation relation;
    relation.type = tokens[i].text;
    relation.kind = canonical(tokens[i + 2].text);
    relation.range = {tokens[i].range.begin, tokens[i + 2].range.end};
    if (tokens[i + 3].text == "(" && tokens[i + 4].kind == TokenKind::Identifier) {
      std::size_t base = i + 4;
      SourceRange baseRange;
      relation.baseType = qualifiedName(tokens, base, &baseRange);
      relation.range.end = baseRange.end;
    }
    ast.inheritance.push_back(std::move(relation));
  }

  for (i = 0; tokens[i].kind != TokenKind::End; ++i) {
    if (!typeOwners.contains(i) || (i > 0 && typeOwners.contains(i - 1))) continue;
    const auto owner = typeOwners.at(i);
    const auto ownerHeader = std::find_if(ast.declarations.begin(), ast.declarations.end(), [&](const AstDeclaration& d) { return d.name == owner; });
    std::string visibility = ownerHeader != ast.declarations.end() && ownerHeader->kind == "interface" ? "public" : "private";
    int depth = 1;
    for (std::size_t n = i; tokens[n].kind != TokenKind::End && depth > 0 && typeOwners.contains(n); ++n) {
      if ((word(tokens[n], "class") || word(tokens[n], "record")) &&
          n > 0 && tokens[n - 1].text == "=") ++depth;
      else if (word(tokens[n], "end")) { --depth; continue; }
      if (depth != 1) continue;
      if (word(tokens[n], "private") || word(tokens[n], "protected") || word(tokens[n], "public") || word(tokens[n], "published")) {
        visibility = canonical(tokens[n].text);
        continue;
      }
      if (word(tokens[n], "procedure") || word(tokens[n], "function") || word(tokens[n], "constructor") || word(tokens[n], "destructor")) {
        if (tokens[n + 1].kind != TokenKind::Identifier || isKeyword(tokens[n + 1].text)) continue;
        AstDeclaration declaration;
        declaration.kind = canonical(tokens[n].text);
        declaration.name = owner + "." + tokens[n + 1].text;
        declaration.ownerType = owner;
        declaration.visibility = visibility;
        declaration.range = {tokens[n].range.begin, tokens[n + 1].range.end};
        std::size_t cursor = n + 2;
        if (tokens[cursor].text == "(") {
          const auto open = cursor++;
          while (tokens[cursor].kind != TokenKind::End && tokens[cursor].text != ")") {
            if (tokens[cursor].text == ":" && cursor + 1 < tokens.size()) {
              std::size_t typeCursor = cursor + 1;
              SourceRange typeRange;
              const auto parameterType = qualifiedName(tokens, typeCursor, &typeRange);
              std::size_t defaultEnd = typeCursor;
              std::optional<std::string> defaultValue;
              if (tokens[defaultEnd].text == "=") {
                const auto first = ++defaultEnd;
                int depth = 0;
                while (tokens[defaultEnd].kind != TokenKind::End) {
                  const auto& value = tokens[defaultEnd].text;
                  if (!depth && (value == ";" || value == ")")) break;
                  if (value == "(" || value == "[") ++depth;
                  else if (value == ")" || value == "]") --depth;
                  ++defaultEnd;
                }
                if (defaultEnd > first) defaultValue = sourceSlice(source, {tokens[first].range.begin, tokens[defaultEnd - 1].range.end});
              }
              std::size_t groupStart = cursor;
              while (groupStart > open + 1 && tokens[groupStart - 1].text != ";") --groupStart;
              std::string modifier;
              if (word(tokens[groupStart], "const") || word(tokens[groupStart], "var") || word(tokens[groupStart], "out")) {
                modifier = canonical(tokens[groupStart].text); ++groupStart;
              }
              for (std::size_t p = groupStart; p < cursor; ++p)
                if (tokens[p].kind == TokenKind::Identifier && !isKeyword(tokens[p].text)) {
                  declaration.parameters.push_back({tokens[p].text, parameterType,
                                                     {tokens[p].range.begin, typeRange.end}, modifier});
                  declaration.parameters.back().defaultValue = defaultValue;
                }
              cursor = defaultEnd - 1;
            }
            ++cursor;
          }
        }
        if (word(tokens[n], "function") && tokens[cursor + 1].text == ":" && tokens[cursor + 2].kind == TokenKind::Identifier)
          declaration.type = tokens[cursor + 2].text;
        while (tokens[cursor].kind != TokenKind::End && tokens[cursor].text != ";") ++cursor;
        for (std::size_t d = cursor + 1; d < tokens.size() && tokens[d].kind != TokenKind::End && d < cursor + 16 &&
             (tokens[d].text == ";" || word(tokens[d], "overload") || word(tokens[d], "override") ||
              word(tokens[d], "reintroduce") || word(tokens[d], "deprecated")); ++d) {
          if (word(tokens[d], "overload")) declaration.overload = true;
          if (word(tokens[d], "override")) declaration.isOverride = true;
          if (word(tokens[d], "reintroduce")) declaration.reintroduced = true;
          if (word(tokens[d], "deprecated")) declaration.deprecated = true;
        }
        ast.declarations.push_back(std::move(declaration));
        while (tokens[cursor].kind != TokenKind::End && tokens[cursor].text != ";") ++cursor;
        n = cursor;
      } else if (word(tokens[n], "property") && tokens[n + 1].kind == TokenKind::Identifier) {
        AstDeclaration declaration;
        declaration.kind = "property";
        declaration.name = owner + "." + tokens[n + 1].text;
        declaration.ownerType = owner;
        declaration.visibility = visibility;
        declaration.range = {tokens[n].range.begin, tokens[n + 1].range.end};
        std::size_t propertyType = n + 2;
        if (tokens[propertyType].text == "[") {
          int brackets = 1;
          while (tokens[++propertyType].kind != TokenKind::End && brackets) {
            if (tokens[propertyType].text == "[") ++brackets;
            else if (tokens[propertyType].text == "]") --brackets;
          }
        }
        if (tokens[propertyType].text == ":" && tokens[propertyType + 1].kind == TokenKind::Identifier) {
          ++propertyType;
          declaration.type = qualifiedName(tokens, propertyType);
        }
        ast.declarations.push_back(std::move(declaration));
        // Property names/types/accessors are not separate field declarations.
        while (tokens[n].kind != TokenKind::End && tokens[n].text != ";") ++n;
      } else if (tokens[n].kind == TokenKind::Identifier && tokens[n + 1].text == ":" && tokens[n + 2].kind == TokenKind::Identifier) {
        AstDeclaration declaration;
        declaration.kind = "field";
        declaration.name = owner + "." + tokens[n].text;
        declaration.ownerType = owner;
        declaration.visibility = visibility;
        declaration.type = tokens[n + 2].text;
        declaration.range = {tokens[n].range.begin, tokens[n + 2].range.end};
        ast.declarations.push_back(std::move(declaration));
      }
    }
  }

  for (i = 0; tokens[i].kind != TokenKind::End; ++i) {
    if (i > 0 && tokens[i - 1].text == ".") continue;
    if (tokens[i].kind == TokenKind::Identifier) {
      std::size_t assignmentOperator = i;
      SourceRange leftRange;
      auto left = qualifiedName(tokens, assignmentOperator, &leftRange);
      // Preserve indexed property/array targets as expressions. Their schema
      // or receiver type still requires an explicit semantic contract.
      if (tokens[assignmentOperator].text == "[") {
        int indexDepth = 1;
        std::size_t close = assignmentOperator + 1;
        while (tokens[close].kind != TokenKind::End && indexDepth) {
          if (tokens[close].text == "[") ++indexDepth;
          else if (tokens[close].text == "]") --indexDepth;
          ++close;
        }
        if (indexDepth == 0) {
          leftRange.end = tokens[close - 1].range.end;
          left = sourceSlice(source, leftRange);
          assignmentOperator = close;
        } else ast.complete = false;
      }
      if (tokens[assignmentOperator].text == ":" && tokens[assignmentOperator + 1].text == "=") {
        std::size_t end = assignmentOperator + 2;
        int parentheses = 0;
        int brackets = 0;
        while (tokens[end].kind != TokenKind::End) {
          if (tokens[end].text == "(") ++parentheses;
          else if (tokens[end].text == ")") --parentheses;
          else if (tokens[end].text == "[") ++brackets;
          else if (tokens[end].text == "]") --brackets;
          if (tokens[end].text == ";" && parentheses == 0 && brackets == 0) break;
          ++end;
        }
        if (end > assignmentOperator + 2) {
          const SourceRange rightRange{tokens[assignmentOperator + 2].range.begin, tokens[end - 1].range.end};
          AstAssignment assignment;
          assignment.left = left;
          assignment.right = sourceSlice(source, rightRange);
          assignment.range = {leftRange.begin, rightRange.end};
          ast.assignments.push_back(std::move(assignment));
        }
      }
    }
    if (tokens[i].kind != TokenKind::Identifier || isKeyword(tokens[i].text)) continue;
    if (i > 0 && (word(tokens[i - 1], "procedure") || word(tokens[i - 1], "function") ||
                  word(tokens[i - 1], "constructor") || word(tokens[i - 1], "destructor"))) continue;
    std::size_t afterName = i;
    SourceRange callRange;
    const auto callName = qualifiedName(tokens, afterName, &callRange);
    if (tokens[afterName].text != "(") continue;
    const auto open = afterName;
    std::size_t close = open + 1;
    int depth = 1;
    while (tokens[close].kind != TokenKind::End && depth > 0) {
      if (tokens[close].text == "(") ++depth;
      else if (tokens[close].text == ")") --depth;
      ++close;
    }
    if (depth != 0) { ast.complete = false; continue; }
    const auto closeIndex = close - 1;
    callRange.end = tokens[closeIndex].range.end;
    AstCall call;
    call.name = callName;
    call.range = callRange;
    std::size_t argumentStart = open + 1;
    int nestedParen = 0;
    int nestedBracket = 0;
    for (std::size_t n = argumentStart; n <= closeIndex; ++n) {
      const bool separator = n == closeIndex ||
          (tokens[n].text == "," && nestedParen == 0 && nestedBracket == 0);
      if (separator && n > argumentStart) {
        const SourceRange range{tokens[argumentStart].range.begin, tokens[n - 1].range.end};
        auto type = builtinType(tokens[argumentStart]);
        if (type.empty() && tokens[argumentStart].kind == TokenKind::Identifier) {
          const auto declared = variableTypes.find(canonical(tokens[argumentStart].text));
          if (declared != variableTypes.end()) type = declared->second;
        }
        call.arguments.push_back({argumentKind(tokens[argumentStart]), sourceSlice(source, range), type, range});
        argumentStart = n + 1;
      } else if (tokens[n].text == "(") ++nestedParen;
      else if (tokens[n].text == ")") --nestedParen;
      else if (tokens[n].text == "[") ++nestedBracket;
      else if (tokens[n].text == "]") --nestedBracket;
    }
    for (const auto& assignment : ast.assignments) {
      if (call.range.begin.offset >= assignment.range.begin.offset &&
          call.range.end.offset <= assignment.range.end.offset) {
        call.assignmentTarget.kind = assignment.left.find('.') == std::string::npos ? "identifier" : "property";
        call.assignmentTarget.text = assignment.left;
        const auto target = variableTypes.find(canonical(assignment.left));
        if (target != variableTypes.end()) call.assignmentTarget.resolvedType = target->second;
      }
    }
    const auto firstDot = call.name.find('.');
    const auto receiver = canonical(call.name.substr(0, firstDot));
    if (const auto type = variableTypes.find(receiver); type != variableTypes.end())
      call.resolvedReturnType = {};
    else if (firstDot != std::string::npos && canonical(call.name.substr(firstDot + 1)) == "create")
      call.resolvedReturnType = call.name.substr(0, firstDot);
    ast.calls.push_back(std::move(call));
  }
  for (auto& assignment : ast.assignments) {
    auto root = assignment.left.substr(0, assignment.left.find('.'));
    if (const auto type = variableTypes.find(canonical(root)); type != variableTypes.end())
      assignment.targetType = type->second;
    for (const auto& call : ast.calls) {
      if (call.range.begin.offset >= assignment.range.begin.offset && call.range.end.offset <= assignment.range.end.offset) {
        assignment.valueKind = "call";
        assignment.resolvedReturnType = call.resolvedReturnType;
        assignment.arguments = call.arguments;
        break;
      }
    }
  }
  // Bound implementation routine scopes by balanced bodies. Declarations in
  // interfaces have no body and must not capture later implementation locals.
  for (auto& declaration : ast.declarations) {
    if (!declaration.ownerType.empty() || declaration.visibility != "private" ||
        !(declaration.kind == "procedure" || declaration.kind == "function" ||
          declaration.kind == "constructor" || declaration.kind == "destructor")) continue;
    auto start = std::find_if(tokens.begin(), tokens.end(), [&](const Token& token) {
      return token.range.begin.offset == declaration.range.begin.offset;
    });
    if (start == tokens.end()) continue;
    auto body = start;
    // Skip nested routine definitions using the already extracted headers.
    while (body != tokens.end() && !word(*body, "begin") && body->kind != TokenKind::End) ++body;
    if (body == tokens.end() || body->kind == TokenKind::End) continue;
    auto end = body;
    int depth = 1;
    while (++end != tokens.end() && end->kind != TokenKind::End) {
      if (word(*end, "begin") || word(*end, "try") || word(*end, "case") || word(*end, "asm")) ++depth;
      else if (word(*end, "end") && --depth == 0) break;
    }
    if (end == tokens.end() || end->kind == TokenKind::End) { ast.complete = false; continue; }
    declaration.bodyRange = {declaration.range.begin, end->range.end};
    // If the first begin belongs to a nested named routine, find the outer
    // body's following begin rather than claiming the nested scope alone.
    bool nested = std::any_of(ast.declarations.begin(), ast.declarations.end(), [&](const AstDeclaration& other) {
      return other.ownerType.empty() && other.range.begin.offset > declaration.range.begin.offset &&
             other.range.begin.offset < body->range.begin.offset &&
             (other.kind == "procedure" || other.kind == "function");
    });
    if (nested) {
      auto outerBody = end + 1;
      while (outerBody != tokens.end() && outerBody->kind != TokenKind::End && !word(*outerBody,"begin")) ++outerBody;
      if (outerBody != tokens.end() && outerBody->kind != TokenKind::End) {
        auto outerEnd = outerBody; depth = 1;
        while (++outerEnd != tokens.end() && outerEnd->kind != TokenKind::End) {
          if (word(*outerEnd,"begin") || word(*outerEnd,"try") || word(*outerEnd,"case")) ++depth;
          else if (word(*outerEnd,"end") && --depth == 0) break;
        }
        if (outerEnd != tokens.end() && outerEnd->kind != TokenKind::End) declaration.bodyRange.end = outerEnd->range.end;
      }
    }
  }
  for (auto& declaration : ast.declarations) {
    if (declaration.isOverride || declaration.name.find('.') == std::string::npos) continue;
    const auto simple = declaration.name.substr(declaration.name.find_last_of('.') + 1);
    declaration.isOverride = std::any_of(ast.declarations.begin(), ast.declarations.end(), [&](const AstDeclaration& candidate) {
      return candidate.isOverride && canonical(candidate.name) == canonical(simple);
    });
  }
  for (std::size_t n = 1; n + 1 < tokens.size(); ++n) {
    if (!(word(tokens[n], "procedure") || word(tokens[n], "function")) ||
        !(tokens[n - 1].text == "=" || tokens[n - 1].text == "(" || tokens[n - 1].text == ",") ||
        word(tokens[n - 1], "to")) continue;
    if (!(word(tokens[n + 1], "begin") || tokens[n + 1].text == "(" || tokens[n + 1].text == ":")) continue;
    std::size_t body = n + 1;
    int headerParentheses = 0;
    while (body < tokens.size() && tokens[body].kind != TokenKind::End && !word(tokens[body], "begin")) {
      if (tokens[body].text == "(") ++headerParentheses;
      else if (tokens[body].text == ")") --headerParentheses;
      if (tokens[body].text == ";" && headerParentheses == 0) break;
      ++body;
    }
    // A procedural type ends its signature with a semicolon and has no
    // executable body. Do not scan into unrelated later routines for a begin.
    if (body < tokens.size() && tokens[body].text == ";") continue;
    if (body >= tokens.size() || tokens[body].kind == TokenKind::End) { ast.complete = false; continue; }
    int depth = 1;
    std::size_t end = body;
    while (++end < tokens.size() && tokens[end].kind != TokenKind::End && depth) {
      if (word(tokens[end], "begin") || word(tokens[end], "try") || word(tokens[end], "case")) ++depth;
      else if (word(tokens[end], "end")) --depth;
    }
    if (depth) { ast.complete = false; ast.parseDiagnostics.push_back({"anonymous_body_incomplete", "Anonymous method body has no matching end", tokens[n].range.begin}); continue; }
    AstDeclaration declaration;
    declaration.name = "<anonymous@" + std::to_string(tokens[n].range.begin.offset) + ">";
    declaration.kind = "anonymousMethod"; declaration.anonymous = true;
    declaration.visibility = "private"; declaration.scope = ast.name;
    declaration.range = {tokens[n].range.begin, tokens[end - 1].range.end};
    // The loop increments once after the terminating end; range is half-open.
    declaration.signature = sourceSlice(source, {tokens[n].range.begin, tokens[body].range.begin});
    for (const auto& routine : ast.declarations)
      if ((routine.kind == "procedure" || routine.kind == "function") && routine.ownerType.empty() &&
          routine.range.begin.offset < tokens[n].range.begin.offset)
        declaration.enclosingRoutine = routine.name;
    ast.declarations.push_back(std::move(declaration));
  }
  for (auto& declaration : ast.declarations) {
    declaration.stringMode = "PROJECT_DEFAULT_UNRESOLVED";
    declaration.rttiMode = "PROJECT_DEFAULT_UNRESOLVED";
    declaration.typedConstantMode = "PROJECT_DEFAULT_UNRESOLVED";
    declaration.typedAddressMode = "PROJECT_DEFAULT_UNRESOLVED";
    declaration.variantPropertySetterMode = "PROJECT_DEFAULT_UNRESOLVED";
    for (const auto& state : semanticSwitches) {
      if (state.offset > declaration.range.begin.offset) continue;
      if (state.name == 'h') declaration.stringMode = state.enabled ? "LONGSTRING" : "SHORTSTRING";
      if (state.name == 'm') declaration.rttiMode = state.enabled ? "ENABLED" : "DISABLED";
      if (state.name == 'j') declaration.typedConstantMode = state.enabled ? "WRITABLE" : "READONLY";
      if (state.name == 't') declaration.typedAddressMode = state.enabled ? "TYPED" : "UNTYPED";
      if (state.name == 'v') declaration.variantPropertySetterMode = state.enabled ? "ENABLED" : "DISABLED";
    }
    if (declaration.stringMode == "SHORTSTRING") {
      if (canonical(declaration.type) == "string") declaration.type = "ShortString";
      for (auto& parameter : declaration.parameters)
        if (canonical(parameter.type) == "string") parameter.type = "ShortString";
    }
  }
  std::unordered_set<std::string> definedTypes;
  for (const auto& d : ast.declarations)
    if (!d.forwardDeclaration) definedTypes.insert(canonical(d.name) + ":" + d.kind);
  // A matching full definition supersedes its forward header.
  ast.declarations.erase(std::remove_if(ast.declarations.begin(), ast.declarations.end(), [&](const AstDeclaration& d) {
    return d.forwardDeclaration && definedTypes.contains(canonical(d.name) + ":" + d.kind);
  }), ast.declarations.end());
  // Keep dotted local/class expressions as evidence. Dropping the receiver
  // can expose a method suffix as an unrelated imported symbol.
  for (std::size_t n = 0; n + 2 < tokens.size(); ++n) {
    if (tokens[n].kind != TokenKind::Identifier || tokens[n + 1].text != "." ||
        tokens[n + 2].kind != TokenKind::Identifier || (n && tokens[n - 1].text == ".")) continue;
    const auto inBody = std::any_of(ast.declarations.begin(), ast.declarations.end(), [&](const AstDeclaration& routine) {
      return routine.bodyRange.end.offset > routine.bodyRange.begin.offset &&
             tokens[n].range.begin.offset >= routine.bodyRange.begin.offset &&
             tokens[n].range.end.offset <= routine.bodyRange.end.offset;
    });
    if (!inBody) continue;
    const auto local = std::any_of(lexicalScopes.begin(), lexicalScopes.end(), [&](const LexicalScope& scope) {
      return tokens[n].range.begin.offset >= scope.begin && tokens[n].range.begin.offset < scope.end &&
             scope.names.contains(canonical(tokens[n].text));
    });
    if (!local && !ownNames.contains(canonical(tokens[n].text))) continue;
    std::size_t end = n;
    SourceRange range;
    const auto name = qualifiedName(tokens, end, &range);
    ast.references.erase(std::remove_if(ast.references.begin(), ast.references.end(), [&](const SymbolReference& ref) {
      return ref.range.begin.offset >= range.begin.offset && ref.range.end.offset <= range.end.offset;
    }), ast.references.end());
    ast.references.push_back({name, range, UsesSection::Implementation, ResolutionStatus::Unresolved, {}, {}, {}, {}, {}, {}, {}, {}});
    n = end - 1;
  }
  return ast;
}

std::string jsonEscape(std::string_view value) {
  return jsonSourceBytes(value);
#if 0
  std::string result;
  for (const char c : value) {
    switch (c) {
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          constexpr char digits[] = "0123456789abcdef";
          result += "\\u00";
          result += digits[(static_cast<unsigned char>(c) >> 4) & 0x0f];
          result += digits[static_cast<unsigned char>(c) & 0x0f];
        } else result += c;
    }
  }
  return result;
#endif
}

}  // namespace

void Analyzer::addSource(std::filesystem::path path, std::string source) {
  inputs_.push_back({std::move(path), std::move(source)});
}

void Analyzer::addIndexedUnit(IndexedUnit unit) { indexedUnits_.push_back(std::move(unit)); }

void Analyzer::addUnitAlias(std::string alias, std::string declaredName) {
  unitAliases_[canonical(alias)] = canonical(declaredName);
}

void Analyzer::setEnvironmentComplete(bool complete) { environmentComplete_ = complete; }

void Analyzer::setPreprocessor(std::vector<std::string> defines,
                              std::vector<std::filesystem::path> includePaths, std::optional<double> compilerVersion) {
  compilerVersion_ = compilerVersion;
  defines_ = std::move(defines);
  includePaths_ = std::move(includePaths);
}

void Analyzer::setBuildContext(std::string configuration, std::string platform) {
  configuration_ = std::move(configuration);
  platform_ = std::move(platform);
}

void Analyzer::addDiagnostic(std::string diagnostic) { diagnostics_.push_back(std::move(diagnostic)); }
void Analyzer::setUnitMappingCatalog(std::filesystem::path catalog, bool valid,
                                     std::vector<UnitMappingSuggestion> mappings) {
  unitMappingCatalog_ = std::move(catalog);
  unitMappingCatalogValid_ = valid;
  unitMappings_ = std::move(mappings);
}

UnitAst parseUnit(std::filesystem::path path, const std::string& source) {
  return parseSource(path, source);
}

std::string sourceFingerprint(std::string_view bytes) {
  return sha256(std::string(bytes));
}

AnalysisResult Analyzer::analyze() const {
  AnalysisResult result;
  result.preprocessorComplete = environmentComplete_;
  result.configuration = configuration_;
  result.platform = platform_;
  result.defines = defines_;
  result.diagnostics = diagnostics_;
  result.unitMappingCatalog = unitMappingCatalog_;
  result.unitMappingCatalogValid = unitMappingCatalogValid_;
  result.unitMappings = unitMappings_;
  result.units.reserve(inputs_.size());
  const PreprocessorOptions preprocessorOptions{defines_, includePaths_, 64, compilerVersion_};
  std::vector<PreprocessResult> sourceMaps;
  for (const auto& input : inputs_) {
    const auto preprocessed = preprocess(input.path, input.source, preprocessorOptions);
    sourceMaps.push_back(preprocessed);
    auto ast = parseSource(input.path, preprocessed.expandedSource);
    ast.sourceHash = sha256(input.source);
    ast.concreteTokens = tokenizeConcrete(input.source).tokens;
    ast.complete = ast.complete && preprocessed.complete;
    // Keep internal expanded ranges until dependency analysis has completed.
    // Serialization remaps them to the original files using this source map.
    ast.includesResolved = preprocessed.includesResolved;
    ast.inactiveRanges = preprocessed.inactiveRanges;
    ast.preprocessorReasons.insert(ast.preprocessorReasons.end(), preprocessed.reasons.begin(), preprocessed.reasons.end());
    if (!ast.complete) result.diagnostics.push_back("source_parse_incomplete: " + input.path.generic_string());
    for (const auto& diagnostic : ast.parseDiagnostics)
      result.diagnostics.push_back(diagnostic.code + ": " + input.path.generic_string() + ":" +
                                   std::to_string(diagnostic.position.line) + ":" + diagnostic.message);
    result.includesResolved.insert(result.includesResolved.end(), preprocessed.includesResolved.begin(),
                                   preprocessed.includesResolved.end());
    result.inactiveRanges.insert(result.inactiveRanges.end(), preprocessed.inactiveRanges.begin(),
                                 preprocessed.inactiveRanges.end());
    result.preprocessorComplete = result.preprocessorComplete && preprocessed.complete;
    result.units.push_back({std::move(ast), {}});
  }
  std::unordered_set<std::string> sourceUnitNames;
  for (const auto& unit : result.units) sourceUnitNames.insert(canonical(unit.ast.name));
  for (const auto& indexed : indexedUnits_) {
    if (sourceUnitNames.contains(canonical(indexed.name))) continue;
    UnitAst ast;
    ast.name = indexed.name;
    ast.file = indexed.sourceFile.empty() ? std::filesystem::path("<index>") : indexed.sourceFile;
    ast.hasInitialization = indexed.hasInitialization;
    ast.hasFinalization = indexed.hasFinalization;
    ast.complete = indexed.complete;
    ast.indexOnly = true;
    ast.sourceIndex = indexed.indexVersion;
    for (const auto& symbol : indexed.symbols)
      ast.exports.push_back({symbol, "indexed", {}});
    ast.declarations = indexed.declarations;
    ast.inheritance = indexed.inheritance;
    for (const auto& dependency : indexed.dependencies)
      ast.uses.push_back({dependency, UsesSection::Interface, {}});
    result.units.push_back({std::move(ast), {}});
  }

  std::unordered_map<std::string, std::vector<std::pair<std::size_t, const SymbolDeclaration*>>> symbols;
  std::unordered_map<std::string, std::size_t> units;
  std::unordered_set<std::string> duplicateUnits;
  for (std::size_t i = 0; i < result.units.size(); ++i) {
    const auto unitName = canonical(result.units[i].ast.name);
    if (!units.emplace(unitName, i).second) duplicateUnits.insert(unitName);
    for (const auto& symbol : result.units[i].ast.exports)
      symbols[canonical(symbol.name)].push_back({i, &symbol});
  }

  struct TypeLocation { std::size_t unit; const AstDeclaration* declaration; };
  struct MemberLocation { std::size_t unit; const AstDeclaration* declaration; };
  auto leafName = [](const std::string& name) { return name.substr(name.find_last_of('.') == std::string::npos ? 0 : name.find_last_of('.') + 1); };
  std::unordered_map<std::string, std::vector<TypeLocation>> types;
  std::unordered_map<std::string, std::vector<MemberLocation>> members;
  for (std::size_t u = 0; u < result.units.size(); ++u) {
    for (const auto& d : result.units[u].ast.declarations) {
      if (d.ownerType.empty() && (d.kind == "class" || d.kind == "interface" || d.kind == "record"))
        types[canonical(d.name)].push_back({u, &d});
      if (!d.ownerType.empty())
        members[std::to_string(u) + ":" + canonical(d.ownerType) + ":" + canonical(leafName(d.name))].push_back({u, &d});
    }
  }
  auto locateType = [&](std::size_t context, const std::string& name, UsesSection section) -> std::optional<TypeLocation> {
    std::string typeName = name, explicitUnit;
    for (const auto& [unitName, index] : units) {
      const auto prefix = unitName + ".";
      if (canonical(name).starts_with(prefix) && unitName.size() > explicitUnit.size()) {
        explicitUnit = unitName;
        typeName = name.substr(prefix.size());
      }
    }
    const auto found = types.find(canonical(typeName));
    if (found == types.end()) return {};
    std::vector<TypeLocation> visible;
    for (const auto& t : found->second) {
      const auto& candidate = result.units[t.unit].ast;
      if (duplicateUnits.contains(canonical(candidate.name)) || t.declaration->forwardDeclaration) continue;
      if (!explicitUnit.empty() && canonical(candidate.name) != explicitUnit) continue;
      if (t.unit == context && explicitUnit.empty()) return t;
      const auto used = std::any_of(result.units[context].ast.uses.begin(), result.units[context].ast.uses.end(), [&](const UsesItem& use) {
        auto unitName = canonical(use.name);
        if (const auto alias = unitAliases_.find(unitName); alias != unitAliases_.end()) unitName = alias->second;
        return unitName == canonical(candidate.name) && !(section == UsesSection::Interface && use.section == UsesSection::Implementation);
      });
      // System is implicitly visible, but members still require real unique
      // declarations in the loaded candidate index.
      if (used || canonical(candidate.name) == "system" || (t.unit == context && !explicitUnit.empty())) visible.push_back(t);
    }
    return visible.size() == 1 ? std::optional<TypeLocation>(visible.front()) : std::nullopt;
  };
  auto lookupMember = [&](auto&& self, TypeLocation type, const std::string& name,
                          std::unordered_set<std::string>& visited) -> std::vector<MemberLocation> {
    const auto key = std::to_string(type.unit) + ":" + canonical(type.declaration->name);
    if (!visited.insert(key).second) return {};
    if (const auto found = members.find(key + ":" + canonical(name)); found != members.end()) return found->second;
    std::vector<MemberLocation> inherited;
    for (const auto& base : result.units[type.unit].ast.inheritance) {
      if (canonical(base.type) != canonical(type.declaration->name) || base.baseType.empty()) continue;
      const auto target = locateType(type.unit, base.baseType, UsesSection::Interface);
      if (!target) return {};
      auto matches = self(self, *target, name, visited);
      inherited.insert(inherited.end(), matches.begin(), matches.end());
    }
    return inherited;
  };
  for (std::size_t u = 0; u < inputs_.size(); ++u) {
    auto& ast = result.units[u].ast;
    if (!ast.complete) continue;
    for (auto& ref : ast.references) {
      const auto dot = ref.name.find('.');
      if (dot == std::string::npos) continue;
      const AstDeclaration* routine = nullptr;
      for (const auto& d : ast.declarations)
        if (d.bodyRange.end.offset > d.bodyRange.begin.offset && d.bodyRange.begin.offset <= ref.range.begin.offset &&
            ref.range.end.offset <= d.bodyRange.end.offset && (!routine || d.bodyRange.begin.offset > routine->bodyRange.begin.offset)) routine = &d;
      if (!routine) continue;
      const bool hasWith = std::any_of(ast.concreteTokens.begin(), ast.concreteTokens.end(), [&](const ConcreteToken& token) {
        return canonical(token.bytes) == "with" && token.range.begin.offset >= routine->bodyRange.begin.offset &&
               token.range.end.offset <= ref.range.begin.offset &&
               !std::any_of(ast.declarations.begin(), ast.declarations.end(), [&](const AstDeclaration& nested) {
                 return &nested != routine && nested.bodyRange.begin.offset > routine->bodyRange.begin.offset &&
                        nested.bodyRange.end.offset <= routine->bodyRange.end.offset &&
                        nested.bodyRange.begin.offset <= token.range.begin.offset && token.range.end.offset <= nested.bodyRange.end.offset;
               });
      });
      const auto receiver = ref.name.substr(0, dot);
      std::string receiverDeclaredType;
      std::size_t typeContext = u;
      bool bound = false;
      bool classReceiver = false;
      std::vector<std::string> localTypes;
      for (const auto& d : ast.declarations)
        if (d.kind == "localVariable" && canonical(d.scope) == canonical(routine->name) && canonical(d.name) == canonical(receiver)) {
          bound = true; localTypes.push_back(d.type);
        }
      if (!bound) for (const auto& p : routine->parameters)
        if (canonical(p.name) == canonical(receiver)) { bound = true; localTypes.push_back(p.type); }
      if (localTypes.size() == 1) receiverDeclaredType = localTypes.front();
      const auto ownerDot = routine->name.find_last_of('.');
      const auto ownerName = ownerDot == std::string::npos ? std::string{} : routine->name.substr(0, ownerDot);
      if (!bound && !ownerName.empty()) {
        if (canonical(receiver) == "self") { bound = true; receiverDeclaredType = ownerName; }
        else if (const auto owner = locateType(u, ownerName, ref.section)) {
          std::unordered_set<std::string> visited;
          auto found = lookupMember(lookupMember, *owner, receiver, visited);
          if (!found.empty()) {
            bound = true;
            if (found.size() == 1 && (found.front().declaration->kind == "field" || found.front().declaration->kind == "property")) {
              receiverDeclaredType = found.front().declaration->type;
              typeContext = found.front().unit;
              ref.receiverDeclaration = result.units[typeContext].ast.name + "." + found.front().declaration->name;
            }
          }
        }
      }
      if (!bound) for (const auto& d : ast.declarations)
        if (d.kind == "variable" && canonical(d.name) == canonical(receiver)) {
          bound = true; receiverDeclaredType = d.type; break;
        }
      if (!bound && locateType(u, receiver, ref.section)) {
        bound = true; classReceiver = true; receiverDeclaredType = receiver;
      }
      if (!bound) continue;
      ref.resolutionMethod = "scoped_member_unresolved";
      ref.receiverType = receiverDeclaredType;
      // A later with cannot shadow this expression; any earlier with requires
      // a dedicated scope model before inferring member ownership.
      if (hasWith || receiverDeclaredType.empty()) continue;
      auto type = locateType(typeContext, receiverDeclaredType, ref.section);
      if (!type) continue;
      std::size_t begin = dot + 1;
      while (begin < ref.name.size()) {
        const auto end = ref.name.find('.', begin);
        std::unordered_set<std::string> visited;
        auto found = lookupMember(lookupMember, *type, ref.name.substr(begin, end == std::string::npos ? end : end - begin), visited);
        if (found.empty()) break;
        if (end == std::string::npos) {
          const auto& first = found.front();
          if (!result.units[first.unit].ast.complete) break;
          if (classReceiver && first.declaration->kind != "constructor") break;
          if (std::any_of(found.begin(), found.end(), [&](const MemberLocation& member) {
            return member.unit != first.unit || canonical(member.declaration->name) != canonical(first.declaration->name);
          })) break;
          if ((first.declaration->visibility == "private" || first.declaration->visibility == "protected") && first.unit != u) break;
          ref.status = ResolutionStatus::Resolved;
          ref.declaringUnit = result.units[first.unit].ast.name;
          ref.resolvedDeclaration = first.declaration->name;
          ref.resolvedKind = first.declaration->kind;
          ref.memberType = found.size() == 1 ? first.declaration->type : std::string{};
          ref.resolutionMethod = found.size() == 1 ? "scoped_member" : "scoped_member_overload_set";
          break;
        }
        if (found.size() != 1 || (found.front().declaration->kind != "field" && found.front().declaration->kind != "property")) break;
        if (!result.units[found.front().unit].ast.complete ||
            ((found.front().declaration->visibility == "private" || found.front().declaration->visibility == "protected") && found.front().unit != u)) break;
        type = locateType(found.front().unit, found.front().declaration->type, UsesSection::Interface);
        if (!type) break;
        begin = end + 1;
      }
    }
  }

  for (auto& analyzed : result.units) {
    if (analyzed.ast.indexOnly) continue;
    const auto context = static_cast<std::size_t>(&analyzed - result.units.data());
    for (auto& call : analyzed.ast.calls) {
      if (call.name.find('.') != std::string::npos) {
        call.resolvedReturnType.clear();
        call.signatureStatus = "MEMBER_UNRESOLVED";
        const auto ref = std::find_if(analyzed.ast.references.begin(), analyzed.ast.references.end(), [&](const SymbolReference& r) {
          return canonical(r.name) == canonical(call.name) && r.range.begin.offset == call.range.begin.offset &&
                 r.status == ResolutionStatus::Resolved && !r.resolvedDeclaration.empty();
        });
        if (ref == analyzed.ast.references.end()) continue;
        call.qualifiedMember = ref->declaringUnit + "." + ref->resolvedDeclaration;
        const auto owner = units.find(canonical(ref->declaringUnit));
        if (owner == units.end() || duplicateUnits.contains(owner->first)) continue;
        std::vector<const AstDeclaration*> declarations;
        for (const auto& d : result.units[owner->second].ast.declarations)
          if (canonical(d.name) == canonical(ref->resolvedDeclaration) &&
              (d.kind == "function" || d.kind == "procedure" || d.kind == "constructor")) declarations.push_back(&d);
        // Reconstruct argument evidence in the enclosing routine. The parser's
        // unit-wide variable map is not sufficient for overload selection.
        const AstDeclaration* routine = nullptr;
        for (const auto& d : analyzed.ast.declarations)
          if (d.bodyRange.end.offset > d.bodyRange.begin.offset && d.bodyRange.begin.offset <= call.range.begin.offset &&
              call.range.end.offset <= d.bodyRange.end.offset && (!routine || d.bodyRange.begin.offset > routine->bodyRange.begin.offset)) routine = &d;
        std::vector<bool> lvalues;
        std::vector<std::size_t> argumentContexts;
        for (auto& arg : call.arguments) {
          arg.resolvedType.clear();
          bool lvalue = false;
          auto argumentContext = context;
          if (std::regex_match(arg.text, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)")) && routine) {
            std::vector<std::string> evidence;
            for (const auto& d : analyzed.ast.declarations)
              if (d.kind == "localVariable" && canonical(d.scope) == canonical(routine->name) && canonical(d.name) == canonical(arg.text))
                evidence.push_back(d.type);
            if (evidence.empty()) for (const auto& p : routine->parameters)
              if (canonical(p.name) == canonical(arg.text)) evidence.push_back(p.type);
            if (evidence.size() == 1) {
              arg.resolvedType = evidence.front(); lvalue = true;
              for (const auto& p : routine->parameters)
                if (canonical(p.name) == canonical(arg.text) && p.modifier == "const") lvalue = false;
            }
          } else {
            const auto argumentRef = std::find_if(analyzed.ast.references.begin(), analyzed.ast.references.end(), [&](const SymbolReference& r) {
              return r.range.begin.offset == arg.range.begin.offset && r.range.end.offset == arg.range.end.offset &&
                     r.status == ResolutionStatus::Resolved && !r.memberType.empty();
            });
            if (argumentRef != analyzed.ast.references.end()) {
              arg.resolvedType = argumentRef->memberType;
              const auto declaring = units.find(canonical(argumentRef->declaringUnit));
              if (declaring != units.end()) argumentContext = declaring->second;
            }
          }
          lvalues.push_back(lvalue);
          argumentContexts.push_back(argumentContext);
        }
        std::vector<const AstDeclaration*> exact;
        const auto signatureText = [](const AstDeclaration& d) {
          if (!d.signature.empty()) return d.signature;
          std::string signature = d.kind + " " + d.name + "(";
          for (std::size_t p = 0; p < d.parameters.size(); ++p) {
            if (p) signature += "; ";
            if (!d.parameters[p].modifier.empty()) signature += d.parameters[p].modifier + " ";
            signature += d.parameters[p].name + ": " + d.parameters[p].type;
            if (d.parameters[p].defaultValue) signature += " = " + *d.parameters[p].defaultValue;
          }
          signature += ")";
          if (!d.type.empty()) signature += ": " + d.type;
          return signature;
        };
        bool uncertain = false;
        bool usedInheritance = false;
        const auto derivedFrom = [&](TypeLocation actual, TypeLocation expected) {
          std::unordered_set<std::string> visited;
          while (true) {
            if (actual.unit == expected.unit && actual.declaration == expected.declaration) return true;
            if (actual.declaration->kind != "class" || expected.declaration->kind != "class") return false;
            if (!result.units[actual.unit].ast.complete || !result.units[expected.unit].ast.complete) return false;
            const auto key = std::to_string(actual.unit) + ":" + canonical(actual.declaration->name);
            if (!visited.insert(key).second) return false;
            std::vector<TypeLocation> bases;
            for (const auto& relation : result.units[actual.unit].ast.inheritance) {
              if (canonical(relation.type) != canonical(actual.declaration->name) || relation.baseType.empty()) continue;
              const auto base = locateType(actual.unit, relation.baseType, UsesSection::Interface);
              if (!base) return false;
              if (std::none_of(bases.begin(), bases.end(), [&](const TypeLocation& b) {
                return b.unit == base->unit && b.declaration == base->declaration;
              })) bases.push_back(*base);
            }
            if (bases.size() != 1) return false;
            actual = bases.front();
          }
        };
        std::size_t arityCandidates = 0;
        for (const auto* d : declarations) {
          if (call.arguments.size() > d->parameters.size()) continue;
          if (std::any_of(d->parameters.begin() + call.arguments.size(), d->parameters.end(), [](const AstParameter& p) { return !p.defaultValue; })) continue;
          ++arityCandidates;
          call.candidateSignatures.push_back(signatureText(*d));
          bool compatible = true;
          for (std::size_t a = 0; a < call.arguments.size(); ++a) {
            const auto& arg = call.arguments[a]; const auto& parameter = d->parameters[a];
            if (arg.resolvedType.empty() || ((parameter.modifier == "var" || parameter.modifier == "out") && !lvalues[a])) {
              uncertain = true; compatible = false; continue;
            }
            // Only exact, unit-qualified class identity and exact scalar
            // identity are supported; Delphi conversions/aliases stay review.
            const auto actual = locateType(argumentContexts[a], arg.resolvedType, UsesSection::Implementation);
            const auto expected = locateType(owner->second, parameter.type, UsesSection::Interface);
            const bool scalar = !actual && !expected && canonical(arg.resolvedType) == canonical(parameter.type) &&
                (canonical(parameter.type) == "integer" || canonical(parameter.type) == "boolean" ||
                 canonical(parameter.type) == "nativeint" || canonical(parameter.type) == "int64");
            const bool identical = actual && expected && actual->unit == expected->unit && actual->declaration == expected->declaration;
            const bool inherited = actual && expected && !identical && parameter.modifier != "var" && parameter.modifier != "out" && derivedFrom(*actual, *expected);
            if (inherited) usedInheritance = true;
            if (!scalar && !identical && !inherited) {
              uncertain = true; compatible = false;
            }
          }
          if (compatible) exact.push_back(d);
        }
        call.signatureStatus = arityCandidates == 0 ? "ARITY_MISMATCH_OR_NONCALLABLE" : "ARGUMENT_COMPATIBILITY_REVIEW";
        if (exact.size() == 1 && !uncertain) {
          call.signatureSelected = true;
          call.selectedSignature = signatureText(*exact.front());
          call.signatureStatus = usedInheritance ? "DECLARED_CLASS_ASSIGNABLE" : "EXACT_DECLARED_TYPES";
          call.resolvedReturnType = exact.front()->kind == "constructor" ? ref->receiverType : exact.front()->type;
        } else if (exact.size() > 1) call.signatureStatus = "OVERLOAD_AMBIGUOUS";
        continue;
      }
      if (!call.resolvedReturnType.empty()) continue;
      auto simple = call.name.substr(call.name.find_last_of('.') == std::string::npos
                                         ? 0 : call.name.find_last_of('.') + 1);
      std::vector<std::string> returnTypes;
      for (const auto& candidateUnit : result.units) {
        const auto visible = candidateUnit.ast.name == analyzed.ast.name ||
            std::any_of(analyzed.ast.uses.begin(), analyzed.ast.uses.end(), [&](const UsesItem& use) {
              auto usedName = canonical(use.name);
              if (const auto alias = unitAliases_.find(usedName); alias != unitAliases_.end()) usedName = alias->second;
              return usedName == canonical(candidateUnit.ast.name);
            });
        if (!visible) continue;
        for (const auto& declaration : candidateUnit.ast.declarations)
          if (canonical(declaration.name.substr(declaration.name.find_last_of('.') == std::string::npos
                                                    ? 0 : declaration.name.find_last_of('.') + 1)) == canonical(simple) &&
              !declaration.type.empty())
            returnTypes.push_back(declaration.type);
      }
      std::sort(returnTypes.begin(), returnTypes.end());
      returnTypes.erase(std::unique(returnTypes.begin(), returnTypes.end()), returnTypes.end());
      if (returnTypes.size() == 1) call.resolvedReturnType = returnTypes.front();
    }
    for (auto& assignment : analyzed.ast.assignments) {
      for (const auto& call : analyzed.ast.calls)
        if (call.range.begin.offset >= assignment.range.begin.offset && call.range.end.offset <= assignment.range.end.offset) {
          assignment.valueKind = "call";
          assignment.resolvedReturnType = call.resolvedReturnType;
          assignment.arguments = call.arguments;
          break;
        }
    }
  }

  for (std::size_t sourceIndex = 0; sourceIndex < result.units.size(); ++sourceIndex) {
    auto& analyzed = result.units[sourceIndex];
    if (analyzed.ast.indexOnly) continue;
    std::unordered_map<std::string, std::unordered_set<std::string>> inheritedMemberCache;
    std::unordered_map<std::size_t, bool> localBindingCache;
    for (const auto& item : analyzed.ast.uses) {
      Dependency dependency{item.name, item.section, DependencyStatus::Unknown, Confidence::Low, {}, {}};
      auto targetName = canonical(item.name);
      if (const auto alias = unitAliases_.find(targetName); alias != unitAliases_.end()) targetName = alias->second;
      const auto targetIt = units.find(targetName);
      if (targetIt == units.end()) {
        dependency.status = DependencyStatus::Unknown;
        dependency.confidence = Confidence::Low;
        dependency.reasons.push_back("source_unit_not_indexed: " + item.name);
        analyzed.dependencies.push_back(std::move(dependency));
        continue;
      }
      if (duplicateUnits.contains(targetName)) {
        dependency.status = DependencyStatus::Unknown;
        dependency.confidence = Confidence::Low;
        dependency.reasons.push_back("duplicate_unit_source: " + item.name);
        analyzed.dependencies.push_back(std::move(dependency));
        continue;
      }
      const auto targetIndex = targetIt->second;
      std::vector<std::string> unresolvedReferences;
      for (auto& reference : analyzed.ast.references) {
        if (!reference.resolutionMethod.empty()) {
          if (reference.status == ResolutionStatus::Resolved && canonical(reference.declaringUnit) == canonical(item.name))
            dependency.references.push_back(reference);
          else if (const auto receiver = locateType(sourceIndex, reference.receiverType, reference.section);
                   receiver && receiver->unit == targetIndex) {
            auto typeReference = reference;
            typeReference.name = reference.receiverType;
            typeReference.status = ResolutionStatus::Resolved;
            typeReference.declaringUnit = item.name;
            typeReference.resolvedDeclaration = receiver->declaration->name;
            typeReference.resolutionMethod = "scoped_receiver_type_only";
            dependency.references.push_back(std::move(typeReference));
          }
          continue;
        }
        if (reference.section == UsesSection::Interface && item.section == UsesSection::Implementation) continue;
        auto simple = reference.name;
        std::string explicitUnit;
        for (const auto& visibleUnit : analyzed.ast.uses) {
          const auto prefix = visibleUnit.name + ".";
          if (canonical(reference.name).starts_with(canonical(prefix)) && prefix.size() > explicitUnit.size()) {
            explicitUnit = visibleUnit.name;
            simple = reference.name.substr(prefix.size());
            const auto memberDot = simple.find('.');
            if (memberDot != std::string::npos) simple.resize(memberDot);
          }
        }
        if (explicitUnit.empty()) {
          const auto dot = simple.find('.');
          if (dot != std::string::npos) simple.resize(dot);
          if (dot != std::string::npos && !localBindingCache.contains(reference.range.begin.offset)) {
          localBindingCache[reference.range.begin.offset] = std::any_of(analyzed.ast.declarations.begin(), analyzed.ast.declarations.end(), [&](const AstDeclaration& d) {
            const auto leaf = d.name.substr(d.name.find_last_of('.') == std::string::npos ? 0 : d.name.find_last_of('.') + 1);
            const auto inRoutine = [&](const std::string& scope) {
              return std::any_of(analyzed.ast.declarations.begin(), analyzed.ast.declarations.end(), [&](const AstDeclaration& routine) {
                return routine.name == scope && routine.bodyRange.begin.offset <= reference.range.begin.offset &&
                       reference.range.end.offset <= routine.bodyRange.end.offset && routine.bodyRange.end.offset > routine.bodyRange.begin.offset;
              });
            };
            const bool ownMember = (d.kind == "field" || d.kind == "property") &&
                std::any_of(analyzed.ast.declarations.begin(), analyzed.ast.declarations.end(), [&](const AstDeclaration& routine) {
                  return routine.name.starts_with(d.ownerType + ".") && inRoutine(routine.name);
                });
            return ((d.kind == "variable" || ownMember || (d.kind == "localVariable" && inRoutine(d.scope))) && canonical(leaf) == canonical(simple)) ||
                (inRoutine(d.name) && std::any_of(d.parameters.begin(), d.parameters.end(), [&](const AstParameter& p) { return canonical(p.name) == canonical(simple); }));
          });
          }
          const bool locallyBound = dot != std::string::npos && localBindingCache[reference.range.begin.offset];
          if (locallyBound) {
            // A source receiver cannot be resolved as a foreign unit's exported
            // same-named variable/type. Its member needs scoped type lookup.
            unresolvedReferences.push_back(reference.name);
            continue;
          }
          if (dot != std::string::npos) {
            std::string currentOwner;
            for (const auto& routine : analyzed.ast.declarations)
              if (routine.bodyRange.begin.offset <= reference.range.begin.offset && reference.range.end.offset <= routine.bodyRange.end.offset &&
                  routine.bodyRange.end.offset > routine.bodyRange.begin.offset && routine.name.find('.') != std::string::npos)
                currentOwner = canonical(routine.name.substr(0, routine.name.find_last_of('.')));
            if (!currentOwner.empty() && !inheritedMemberCache.contains(currentOwner)) {
            std::unordered_set<std::string> ancestors;
            ancestors.insert(currentOwner);
            bool changed = true;
            while (changed) {
              changed = false;
              for (const auto& unit : result.units) for (const auto& relation : unit.ast.inheritance)
                if (ancestors.contains(canonical(relation.type)) && !relation.baseType.empty() && ancestors.insert(canonical(relation.baseType)).second) changed = true;
            }
            auto& names = inheritedMemberCache[currentOwner];
            for (const auto& unit : result.units) for (const auto& member : unit.ast.declarations)
              if ((member.kind == "property" || member.kind == "field") && ancestors.contains(canonical(member.ownerType)))
                names.insert(canonical(member.name.substr(member.name.find_last_of('.') + 1)));
            }
            if (!currentOwner.empty() && inheritedMemberCache[currentOwner].contains(canonical(simple))) { unresolvedReferences.push_back(reference.name); continue; }
          }
        }
        if (isBuiltin(simple)) {
          reference.status = ResolutionStatus::Builtin;
          continue;
        }
        const auto candidates = symbols.find(canonical(simple));
        if (candidates == symbols.end()) {
          unresolvedReferences.push_back(reference.name);
          continue;
        }
        std::vector<std::size_t> visible;
        for (const auto& candidate : candidates->second) {
          const auto used = std::find_if(analyzed.ast.uses.begin(), analyzed.ast.uses.end(), [&](const UsesItem& usedItem) {
            auto usedName = canonical(usedItem.name);
            if (const auto alias = unitAliases_.find(usedName); alias != unitAliases_.end()) usedName = alias->second;
            return usedName == canonical(result.units[candidate.first].ast.name) &&
                   !(reference.section == UsesSection::Interface && usedItem.section == UsesSection::Implementation);
          });
          if (used != analyzed.ast.uses.end() &&
              (explicitUnit.empty() || canonical(explicitUnit) == canonical(result.units[candidate.first].ast.name)))
            visible.push_back(candidate.first);
        }
        std::sort(visible.begin(), visible.end());
        visible.erase(std::unique(visible.begin(), visible.end()), visible.end());
        if (visible.size() == 1) {
          reference.status = ResolutionStatus::Resolved;
          reference.declaringUnit = result.units[visible.front()].ast.name;
          if (visible.front() == targetIndex) dependency.references.push_back(reference);
        } else if (visible.size() > 1) {
          reference.status = ResolutionStatus::Ambiguous;
          for (const auto candidate : visible) reference.candidates.push_back(result.units[candidate].ast.name);
          if (std::find(visible.begin(), visible.end(), targetIndex) != visible.end())
            dependency.reasons.push_back("ambiguous_symbol: " + reference.name);
        }
      }
      if (!dependency.references.empty()) {
        dependency.status = DependencyStatus::Used;
        dependency.confidence = Confidence::High;
        if (result.units[targetIndex].ast.sourceIndex == "v500")
          for (const auto& reference : dependency.references)
            dependency.reasons.push_back("legacy_unmapped_symbol:" + reference.name);
      } else if (result.units[targetIndex].ast.hasInitialization || result.units[targetIndex].ast.hasFinalization) {
        dependency.status = DependencyStatus::Used;
        dependency.confidence = Confidence::High;
        if (result.units[targetIndex].ast.hasInitialization) dependency.reasons.push_back("initialization_side_effect");
        if (result.units[targetIndex].ast.hasFinalization) dependency.reasons.push_back("finalization_side_effect");
        dependency.reasons.insert(dependency.reasons.end(), result.units[targetIndex].ast.sideEffectReasons.begin(),
                                  result.units[targetIndex].ast.sideEffectReasons.end());
      } else if (!dependency.reasons.empty() || !environmentComplete_ || !analyzed.ast.complete ||
                 !result.units[targetIndex].ast.complete) {
        dependency.status = DependencyStatus::Unknown;
        dependency.confidence = Confidence::Low;
        if (!environmentComplete_) dependency.reasons.push_back("build_environment_incomplete");
        if (!analyzed.ast.complete) {
          dependency.reasons.push_back("source_parse_incomplete: " + analyzed.ast.name);
          dependency.reasons.insert(dependency.reasons.end(), analyzed.ast.preprocessorReasons.begin(),
                                    analyzed.ast.preprocessorReasons.end());
        }
        if (!result.units[targetIndex].ast.complete)
          dependency.reasons.push_back("source_unit_not_fully_indexed: " + item.name);
        std::sort(unresolvedReferences.begin(), unresolvedReferences.end());
        unresolvedReferences.erase(std::unique(unresolvedReferences.begin(), unresolvedReferences.end()),
                                   unresolvedReferences.end());
        if (!result.units[targetIndex].ast.complete)
          for (const auto& reference : unresolvedReferences)
            dependency.reasons.push_back("unresolved_symbol: " + reference);
      } else {
        dependency.status = DependencyStatus::Unused;
        dependency.confidence = Confidence::High;
        dependency.reasons.push_back("no_references");
      }
      analyzed.dependencies.push_back(std::move(dependency));
    }
  }

  for (const auto& unit : result.units) {
    for (const auto& dependency : unit.dependencies) {
      result.graph.push_back({unit.ast.name, dependency.unit, dependency.section, dependency.status});
    }
  }

  enum class Mark { None, Visiting, Done };
  std::vector<Mark> marks(result.units.size());
  std::vector<std::size_t> stack;
  const auto visit = [&](auto&& self, std::size_t node) -> void {
    marks[node] = Mark::Visiting;
    stack.push_back(node);
    for (const auto& dependency : result.units[node].dependencies) {
      const auto next = units.find(canonical(dependency.unit));
      if (next == units.end()) continue;
      if (marks[next->second] == Mark::None) self(self, next->second);
      else if (marks[next->second] == Mark::Visiting) {
        std::vector<std::string> cycle;
        const auto first = std::find(stack.begin(), stack.end(), next->second);
        for (auto it = first; it != stack.end(); ++it) cycle.push_back(result.units[*it].ast.name);
        cycle.push_back(result.units[next->second].ast.name);
        result.cycles.push_back(std::move(cycle));
      }
    }
    stack.pop_back();
    marks[node] = Mark::Done;
  };
  for (std::size_t i = 0; i < result.units.size(); ++i)
    if (marks[i] == Mark::None) visit(visit, i);
  for (std::size_t u = 0; u < sourceMaps.size(); ++u) {
    auto& ast = result.units[u].ast;
    const auto& origins = sourceMaps[u].origins;
    auto remap = [&](SourceRange& range) {
      if (origins.empty() || range.begin.offset >= origins.size() || range.end.offset >= origins.size()) {
        ast.complete = false; return;
      }
      const auto begin = origins[range.begin.offset];
      auto end = origins[range.end.offset];
      if (end.file != begin.file && range.end.offset > range.begin.offset) {
        end = origins[range.end.offset - 1];
        ++end.position.offset; ++end.position.column;
      }
      if (end.file != begin.file) {
        ast.complete = false;
        ast.preprocessorReasons.push_back("cross_file_span_requires_review");
        return;
      }
      range.begin = begin.position; range.end = end.position; range.sourceFile = begin.file;
    };
    for (auto& declaration : ast.declarations) {
      remap(declaration.range); declaration.sourceFile = declaration.range.sourceFile;
      if (declaration.bodyRange.end.offset > declaration.bodyRange.begin.offset) remap(declaration.bodyRange);
      for (auto& parameter : declaration.parameters) remap(parameter.range);
    }
    for (auto& item : ast.exports) remap(item.range);
    for (auto& item : ast.uses) remap(item.range);
    for (auto& item : ast.references) remap(item.range);
    for (auto& item : ast.inheritance) remap(item.range);
    for (auto& call : ast.calls) { remap(call.range); for (auto& arg : call.arguments) remap(arg.range); }
    for (auto& item : ast.assignments) { remap(item.range); for (auto& arg : item.arguments) remap(arg.range); }
    for (auto& dep : result.units[u].dependencies) for (auto& reference : dep.references) remap(reference.range);
  }
  return result;
}

const char* toString(DependencyStatus value) {
  switch (value) {
    case DependencyStatus::Used: return "USED";
    case DependencyStatus::Unused: return "UNUSED";
    case DependencyStatus::SideEffect: return "SIDE-EFFECT";
    case DependencyStatus::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* toString(Confidence value) {
  switch (value) {
    case Confidence::High: return "HIGH";
    case Confidence::Medium: return "MEDIUM";
    case Confidence::Low: return "LOW";
  }
  return "LOW";
}

std::string toText(const AnalysisResult& result) {
  std::ostringstream output;
  for (const auto& unit : result.units) {
    if (unit.ast.indexOnly) continue;
    output << unit.ast.file.string() << "\nUSES ANALYSIS\n";
    for (const auto& dependency : unit.dependencies) {
      output << dependency.unit << "\t" << toString(dependency.status) << "\t"
             << dependency.references.size() << " references\n";
    }
    output << "Recommendation:\n";
    for (const auto& dependency : unit.dependencies)
      if (dependency.status == DependencyStatus::Unused && dependency.confidence == Confidence::High)
        output << "  REMOVE " << dependency.unit << '\n';
    output << '\n';
  }
  return output.str();
}

std::string toJson(const AnalysisResult& result) {
  std::ostringstream output;
  output << "{\"schemaVersion\":1,\"schema_version\":\"2.1\",\"units\":[";
  bool firstUnit = true;
  for (const auto& unit : result.units) {
    if (unit.ast.indexOnly) continue;
    if (!firstUnit) output << ',';
    firstUnit = false;
    output << "{\"unit\":\"" << jsonEscape(unit.ast.name) << "\",\"file\":\""
           << jsonEscape(unit.ast.file.string()) << "\",\"dependencies\":[";
    for (std::size_t d = 0; d < unit.dependencies.size(); ++d) {
      if (d) output << ',';
      const auto& dependency = unit.dependencies[d];
      output << "{\"unit\":\"" << jsonEscape(dependency.unit) << "\",\"section\":\""
             << (dependency.section == UsesSection::Interface ? "interface" : "implementation")
             << "\",\"status\":\"" << toString(dependency.status) << "\",\"referenceCount\":"
             << dependency.references.size() << ",\"confidence\":\"" << toString(dependency.confidence)
             << "\",\"reasons\":[";
      for (std::size_t reason = 0; reason < dependency.reasons.size(); ++reason) {
        if (reason) output << ',';
        output << '"' << jsonEscape(dependency.reasons[reason]) << '"';
      }
      output << "],\"references\":[";
      for (std::size_t r = 0; r < dependency.references.size(); ++r) {
        if (r) output << ',';
        const auto& reference = dependency.references[r];
        output << "{\"name\":\"" << jsonEscape(reference.name) << "\",\"startOffset\":"
               << reference.range.begin.offset << ",\"endOffset\":" << reference.range.end.offset
               << ",\"line\":" << reference.range.begin.line << ",\"column\":"
               << reference.range.begin.column << '}';
      }
      output << "]}";
    }
    output << "]}";
  }
  output << "],\"project\":{},\"uses\":[],\"references\":[],\"symbols\":[],\"inheritance\":[],\"dependencies\":[],\"diagnostics\":[],\"graph\":[";
  for (std::size_t i = 0; i < result.graph.size(); ++i) {
    if (i) output << ',';
    const auto& edge = result.graph[i];
    output << "{\"from\":\"" << jsonEscape(edge.fromUnit) << "\",\"to\":\""
           << jsonEscape(edge.toUnit) << "\",\"section\":\""
           << (edge.section == UsesSection::Interface ? "interface" : "implementation")
           << "\",\"status\":\"" << toString(edge.status) << "\"}";
  }
  output << "],\"cycles\":[";
  for (std::size_t i = 0; i < result.cycles.size(); ++i) {
    if (i) output << ',';
    output << '[';
    for (std::size_t n = 0; n < result.cycles[i].size(); ++n) {
      if (n) output << ',';
      output << '"' << jsonEscape(result.cycles[i][n]) << '"';
    }
    output << ']';
  }
  output << "]}";
  return output.str();
}

std::string toProjectJson(const AnalysisResult& result, std::string_view plugin,
                          const std::filesystem::path& sourceRoot, std::string_view generatedAt) {
  const auto writePosition = [](std::ostringstream& output, SourceRange range) {
    output << "\"line\":" << range.begin.line << ",\"column\":" << range.begin.column
           << ",\"startOffset\":" << range.begin.offset << ",\"endOffset\":" << range.end.offset;
  };
  std::ostringstream output;
  output << "{\"schemaVersion\":1,\"schema_version\":\"2.1\",\"plugin\":\"" << jsonEscape(plugin)
         << "\",\"sourceRoot\":\"" << jsonEscape(sourceRoot.string())
         << "\",\"generatedAt\":\"" << jsonEscape(generatedAt)
         << "\",\"project\":{\"name\":\"" << jsonEscape(plugin) << "\",\"source_root\":\""
         << jsonEscape(sourceRoot.string()) << "\",\"configuration\":\"" << jsonEscape(result.configuration)
         << "\",\"platform\":\"" << jsonEscape(result.platform) << "\"},\"units\":[";
  bool firstUnit = true;
  for (const auto& unit : result.units) {
    if (unit.ast.indexOnly) continue;
    if (!firstUnit) output << ',';
    firstUnit = false;
    std::error_code error;
    auto relative = std::filesystem::relative(unit.ast.file, sourceRoot, error);
    if (error) relative = unit.ast.file.filename();
    output << "{\"unit\":\"" << jsonEscape(unit.ast.name) << "\",\"file\":\""
           << jsonEscape(relative.generic_string()) << "\",\"sourceHash\":\""
           << unit.ast.sourceHash << "\",\"uses\":{\"interface\":[";
    for (int sectionValue = 0; sectionValue < 2; ++sectionValue) {
      const auto section = sectionValue == 0 ? UsesSection::Interface : UsesSection::Implementation;
      bool firstDependency = true;
      for (const auto& dependency : unit.dependencies) {
        if (dependency.section != section) continue;
        if (!firstDependency) output << ',';
        firstDependency = false;
        output << "{\"unit\":\"" << jsonEscape(dependency.unit) << "\",\"status\":\""
               << toString(dependency.status) << "\",\"confidence\":\""
               << toString(dependency.confidence) << "\",\"reasons\":[";
        for (std::size_t reason = 0; reason < dependency.reasons.size(); ++reason) {
          if (reason) output << ',';
          output << '"' << jsonEscape(dependency.reasons[reason]) << '"';
        }
        output << "],\"references\":[";
        for (std::size_t r = 0; r < dependency.references.size(); ++r) {
          if (r) output << ',';
          const auto& reference = dependency.references[r];
          output << "{\"name\":\"" << jsonEscape(reference.name) << "\",";
          writePosition(output, reference.range);
          output << '}';
        }
        output << "]}";
      }
      output << (sectionValue == 0 ? "],\"implementation\":[" : "]},\"declarations\":[");
    }
    for (std::size_t d = 0; d < unit.ast.declarations.size(); ++d) {
      if (d) output << ',';
      const auto& declaration = unit.ast.declarations[d];
      output << "{\"kind\":\"" << jsonEscape(declaration.kind) << "\",\"name\":\""
             << jsonEscape(declaration.name) << "\",\"visibility\":\""
             << jsonEscape(declaration.visibility) << "\"";
      if (!declaration.type.empty()) output << ",\"type\":\"" << jsonEscape(declaration.type) << "\"";
       if (!declaration.scope.empty()) output << ",\"scope\":\"" << jsonEscape(declaration.scope) << "\"";
       if (declaration.anonymous) output << ",\"anonymous\":true,\"enclosing_routine\":\"" << jsonEscape(declaration.enclosingRoutine) << "\"";
       output << ",\"source_file\":\"" << jsonEscape((declaration.sourceFile.empty() ? unit.ast.file : declaration.sourceFile).generic_string())
              << "\",\"offset_unit\":\"byte\",\"column_unit\":\"byte\"";
      output << ',';
      writePosition(output, declaration.range);
      if (declaration.bodyRange.end.offset > declaration.bodyRange.begin.offset) {
        output << ",\"body_range\":{";
        writePosition(output, declaration.bodyRange);
        output << ",\"source_file\":\"" << jsonEscape(declaration.bodyRange.sourceFile.generic_string()) << "\",\"offset_unit\":\"byte\"}";
      }
      output << ",\"compiler_switches\":{\"string_mode\":\"" << declaration.stringMode
             << "\",\"rtti_mode\":\"" << declaration.rttiMode
             << "\",\"typed_constant_mode\":\"" << declaration.typedConstantMode
             << "\",\"typed_address_mode\":\"" << declaration.typedAddressMode
             << "\",\"variant_property_setter_mode\":\"" << declaration.variantPropertySetterMode << "\"}";
      output << ",\"parameters\":[";
      for (std::size_t p = 0; p < declaration.parameters.size(); ++p) {
        if (p) output << ',';
        const auto& parameter = declaration.parameters[p];
        output << "{\"name\":\"" << jsonEscape(parameter.name) << "\",\"type\":\""
               << jsonEscape(parameter.type) << "\",";
        output << "\"has_default\":" << (parameter.defaultValue ? "true" : "false") << ",\"default_value\":";
        if (parameter.defaultValue) output << '"' << jsonEscape(*parameter.defaultValue) << '"'; else output << "null";
        output << ',';
        writePosition(output, parameter.range);
        output << '}';
      }
      output << "]}";
    }
    output << "],\"trivia\":[";
    bool firstTrivia = true;
    for (const auto& token : unit.ast.concreteTokens) {
      if (token.kind != "comment" && token.kind != "directive") continue;
      if (!firstTrivia) output << ',';
      firstTrivia = false;
      output << "{\"kind\":\"" << token.kind << "\",\"text\":\"" << jsonEscape(token.bytes) << "\",";
      writePosition(output, token.range);
      output << '}';
    }
    output << "],\"calls\":[";
    for (std::size_t c = 0; c < unit.ast.calls.size(); ++c) {
      if (c) output << ',';
      const auto& call = unit.ast.calls[c];
      output << "{\"name\":\"" << jsonEscape(call.name) << "\",";
      writePosition(output, call.range);
      output << ",\"source_file\":\"" << jsonEscape((call.range.sourceFile.empty() ? unit.ast.file : call.range.sourceFile).generic_string())
             << "\",\"offset_unit\":\"byte\"";
      output << ",\"arguments\":[";
      for (std::size_t a = 0; a < call.arguments.size(); ++a) {
        if (a) output << ',';
        const auto& argument = call.arguments[a];
        output << "{\"kind\":\"" << jsonEscape(argument.kind) << "\",\"text\":\""
               << jsonEscape(argument.text) << "\"";
         if (!argument.resolvedType.empty())
           output << ",\"resolvedType\":\"" << jsonEscape(argument.resolvedType) << "\"";
         output << ',';
         writePosition(output, argument.range);
         output << ",\"source_file\":\"" << jsonEscape((argument.range.sourceFile.empty() ? unit.ast.file : argument.range.sourceFile).generic_string())
                << "\",\"offset_unit\":\"byte\"";
         output << '}';
      }
      output << ']';
      if (!call.assignmentTarget.text.empty()) {
        output << ",\"assignmentTarget\":{\"kind\":\"" << jsonEscape(call.assignmentTarget.kind)
               << "\",\"text\":\"" << jsonEscape(call.assignmentTarget.text) << "\"";
        if (!call.assignmentTarget.resolvedType.empty())
          output << ",\"resolvedType\":\"" << jsonEscape(call.assignmentTarget.resolvedType) << "\"";
        output << '}';
      } else output << ",\"assignmentTarget\":null";
      if (!call.resolvedReturnType.empty())
        output << ",\"resolvedReturnType\":\"" << jsonEscape(call.resolvedReturnType) << "\"";
      if (!call.signatureStatus.empty()) {
        output << ",\"qualified_member\":\"" << jsonEscape(call.qualifiedMember)
               << "\",\"signature_status\":\"" << jsonEscape(call.signatureStatus)
               << "\",\"signature_selected\":" << (call.signatureSelected ? "true" : "false")
               << ",\"selected_signature\":\"" << jsonEscape(call.selectedSignature) << "\",\"candidate_signatures\":[";
        for (std::size_t s = 0; s < call.candidateSignatures.size(); ++s) {
          if (s) output << ',';
          output << '"' << jsonEscape(call.candidateSignatures[s]) << '"';
        }
        output << ']';
      }
      output << '}';
    }
    output << "],\"assignments\":[";
    for (std::size_t a = 0; a < unit.ast.assignments.size(); ++a) {
      if (a) output << ',';
      const auto& assignment = unit.ast.assignments[a];
      output << "{\"kind\":\"assignment\",\"target\":{\"text\":\""
             << jsonEscape(assignment.left) << "\"";
      if (!assignment.targetType.empty()) output << ",\"resolvedType\":\"" << jsonEscape(assignment.targetType) << "\"";
      output << "},\"value\":{\"kind\":\"" << jsonEscape(assignment.valueKind.empty() ? "expression" : assignment.valueKind)
             << "\",\"text\":\"" << jsonEscape(assignment.right) << "\"";
      if (!assignment.resolvedReturnType.empty())
        output << ",\"resolvedReturnType\":\"" << jsonEscape(assignment.resolvedReturnType) << "\"";
      output << ",\"arguments\":[";
      for (std::size_t argumentIndex = 0; argumentIndex < assignment.arguments.size(); ++argumentIndex) {
        if (argumentIndex) output << ',';
        const auto& argument = assignment.arguments[argumentIndex];
        output << "{\"text\":\"" << jsonEscape(argument.text) << "\"";
        if (!argument.resolvedType.empty()) output << ",\"resolvedType\":\"" << jsonEscape(argument.resolvedType) << "\"";
        output << '}';
      }
      output << "]},";
      writePosition(output, assignment.range);
      output << ",\"source_file\":\"" << jsonEscape((assignment.range.sourceFile.empty() ? unit.ast.file : assignment.range.sourceFile).generic_string())
             << "\",\"offset_unit\":\"byte\"";
      output << '}';
    }
    output << "],\"unit_mapping\":{\"available\":" << (result.unitMappingCatalogValid ? "true" : "false")
           << ",\"catalog\":";
    if (result.unitMappingCatalog.empty()) output << "null";
    else output << '"' << jsonEscape(result.unitMappingCatalog.generic_string()) << '"';
    output << ",\"suggestions\":[";
    bool firstSuggestion = true;
    for (const auto& mapping : result.unitMappings) {
      const auto used = std::any_of(unit.dependencies.begin(), unit.dependencies.end(), [&](const Dependency& dependency) {
        return canonical(dependency.unit) == canonical(mapping.sourceUnit);
      });
      if (!used) continue;
      if (!firstSuggestion) output << ',';
      firstSuggestion = false;
      output << "{\"source_unit\":\"" << jsonEscape(mapping.sourceUnit) << "\",\"target_unit\":";
      if (mapping.targetUnit.empty()) output << "null"; else output << '"' << jsonEscape(mapping.targetUnit) << '"';
      output << ",\"confidence\":\"" << jsonEscape(mapping.confidence)
             << "\",\"compatibility\":\"" << jsonEscape(mapping.compatibility)
             << "\",\"automatic_action\":\"" << jsonEscape(mapping.automaticAction) << "\"}";
    }
    output << "]}}";
  }
  output << "],\"uses\":[";
  bool firstUseV2 = true;
  for (const auto& unit : result.units) {
    if (unit.ast.indexOnly) continue;
    for (const auto& dependency : unit.dependencies) {
      if (!firstUseV2) output << ',';
      firstUseV2 = false;
      const auto unresolvedCount = std::count_if(dependency.reasons.begin(), dependency.reasons.end(), [](const std::string& reason) {
        return reason.starts_with("unresolved_symbol:") || reason.starts_with("legacy_unmapped_symbol:");
      });
      const auto indexed = std::none_of(dependency.reasons.begin(), dependency.reasons.end(), [](const std::string& reason) {
        return reason.starts_with("source_unit_not_indexed:");
      });
      const auto target = std::find_if(result.units.begin(), result.units.end(), [&](const UnitAnalysis& candidate) {
        return canonical(candidate.ast.name) == canonical(dependency.unit) ||
               (candidate.ast.name.size() > dependency.unit.size() &&
                canonical(candidate.ast.name).ends_with("." + canonical(dependency.unit)));
      });
      const auto allowed = dependency.status == DependencyStatus::Unused && dependency.confidence == Confidence::High &&
                           dependency.reasons.size() == 1 && dependency.reasons.front() == "no_references";
      const auto legacyUnmapped = std::find_if(dependency.reasons.begin(), dependency.reasons.end(), [](const std::string& reason) {
        return reason.starts_with("legacy_unmapped_symbol:");
      });
      const auto removalStatus = legacyUnmapped == dependency.reasons.end() ? toString(dependency.status) : "USED";
      output << "{\"file\":\"" << jsonEscape(unit.ast.file.generic_string()) << "\",\"owner_unit\":\""
             << jsonEscape(unit.ast.name) << "\",\"unit\":\"" << jsonEscape(dependency.unit)
             << "\",\"normalized_unit\":\"" << jsonEscape(canonical(dependency.unit))
             << "\",\"section\":\"" << (dependency.section == UsesSection::Interface ? "interface" : "implementation")
             << "\",\"resolved_source_path\":";
      if (target == result.units.end() || target->ast.file == "<index>") output << "null";
      else output << '"' << jsonEscape(target->ast.file.generic_string()) << '"';
      output << ",\"index_status\":\""
             << (indexed ? "indexed" : "source_unit_not_indexed") << "\",\"usage_summary\":{\"interface_references\":"
             << (dependency.section == UsesSection::Interface ? dependency.references.size() : 0)
             << ",\"implementation_references\":" << (dependency.section == UsesSection::Implementation ? dependency.references.size() : 0)
             << ",\"qualified_references\":0,\"unresolved_possible_references\":" << unresolvedCount
             << ",\"active_code_references\":" << dependency.references.size() << "},\"referenced_symbols\":[";
      for (std::size_t r = 0; r < dependency.references.size(); ++r) {
        if (r) output << ',';
        const auto& reference = dependency.references[r];
        const auto legacy = std::find(dependency.reasons.begin(), dependency.reasons.end(),
                                      "legacy_unmapped_symbol:" + reference.name) != dependency.reasons.end();
        output << "{\"name\":\"" << jsonEscape(reference.name) << "\",\"line\":" << reference.range.begin.line
               << ",\"kind\":\"unknown\",\"resolved\":" << (legacy ? "false" : "true");
        if (legacy) output << ",\"diagnostic\":\"legacy_symbol_unmapped\"";
        output << '}';
      }
      if (legacyUnmapped != dependency.reasons.end()) {
        const auto symbol = legacyUnmapped->substr(legacyUnmapped->find(':') + 1);
        if (!dependency.references.empty()) output << ',';
        output << "{\"name\":\"" << jsonEscape(symbol) << "\",\"line\":0,\"kind\":\"unknown\","
                  "\"resolved\":false,\"diagnostic\":\"legacy_symbol_unmapped\"}";
      }
      output << "],\"removal\":{\"allowed\":" << (allowed ? "true" : "false") << ",\"status\":\""
             << removalStatus << "\",\"confidence\":\"" << toString(dependency.confidence)
             << "\",\"reasons\":[";
      for (std::size_t r = 0; r < dependency.reasons.size(); ++r) { if (r) output << ','; output << '"' << jsonEscape(dependency.reasons[r]) << '"'; }
      output << "]}}";
    }
  }
  output << "],\"references\":[";
  bool firstReferenceV2 = true;
  for (const auto& unit : result.units) {
    if (unit.ast.indexOnly) continue;
    for (const auto& reference : unit.ast.references) {
      if (!firstReferenceV2) output << ',';
      firstReferenceV2 = false;
      const auto resolved = reference.status == ResolutionStatus::Resolved;
      const auto owner = std::find_if(result.units.begin(), result.units.end(), [&](const UnitAnalysis& candidate) {
        return canonical(candidate.ast.name) == canonical(reference.declaringUnit);
      });
      std::string resolvedKind = "unknown";
      if (owner != result.units.end()) {
        auto simple = reference.name.substr(reference.name.find_last_of('.') == std::string::npos
                                                ? 0 : reference.name.find_last_of('.') + 1);
        const auto declaration = std::find_if(owner->ast.exports.begin(), owner->ast.exports.end(), [&](const SymbolDeclaration& symbol) {
          return canonical(symbol.name) == canonical(simple);
        });
        if (declaration != owner->ast.exports.end()) resolvedKind = declaration->kind;
      }
      std::string usage = "type_reference";
      if (std::any_of(unit.ast.calls.begin(), unit.ast.calls.end(), [&](const AstCall& call) {
            return reference.range.begin.offset >= call.range.begin.offset && reference.range.begin.offset < call.range.end.offset;
          })) usage = "invocation";
      else if (std::any_of(unit.ast.assignments.begin(), unit.ast.assignments.end(), [&](const AstAssignment& assignment) {
                 return reference.range.begin.offset >= assignment.range.begin.offset &&
                        reference.range.end.offset <= assignment.range.end.offset &&
                        reference.name == assignment.left;
               })) usage = "write";
      output << "{\"file\":\"" << jsonEscape(unit.ast.file.generic_string()) << "\",\"unit\":\""
             << jsonEscape(unit.ast.name) << "\",\"line\":" << reference.range.begin.line << ",\"column\":"
             << reference.range.begin.column << ",\"symbol\":\"" << jsonEscape(reference.name)
             << "\",\"qualified_name\":";
      if (resolved) output << '"' << jsonEscape(reference.declaringUnit + "." + (reference.resolvedDeclaration.empty() ? reference.name : reference.resolvedDeclaration)) << '"'; else output << "null";
      output << ",\"kind\":\"" << jsonEscape(reference.resolvedKind.empty() ? resolvedKind : reference.resolvedKind) << "\",\"usage\":\"" << usage << "\",\"declared_in_unit\":";
      if (resolved) output << '"' << jsonEscape(reference.declaringUnit) << '"'; else output << "null";
      output << ",\"declared_in_file\":";
      if (owner == result.units.end() || owner->ast.file == "<index>") output << "null";
      else output << '"' << jsonEscape(owner->ast.file.generic_string()) << '"';
      output << ",\"resolved\":" << (resolved ? "true" : "false")
             << ",\"confidence\":\"" << (resolved ? "high" : "low") << "\",\"source_index\":\""
             << (resolved ? (owner != result.units.end() ? owner->ast.sourceIndex : "project") : "unknown") << "\"";
      if (!reference.resolutionMethod.empty())
        output << ",\"resolution_method\":\"" << jsonEscape(reference.resolutionMethod)
               << "\",\"receiver_type\":\"" << jsonEscape(reference.receiverType)
               << "\",\"receiver_declaration\":\"" << jsonEscape(reference.receiverDeclaration)
               << "\",\"member_type\":\"" << jsonEscape(reference.memberType) << "\",\"signature_selected\":false";
      if (!resolved) {
        output << ",\"diagnostic\":\"" << (reference.status == ResolutionStatus::Ambiguous ? "ambiguous_symbol" : "unresolved_symbol")
               << "\",\"candidates\":[";
        for (std::size_t c = 0; c < reference.candidates.size(); ++c) {
          if (c) output << ',';
          output << '"' << jsonEscape(reference.candidates[c]) << '"';
        }
        output << ']';
      }
      output << '}';
    }
  }
  output << "],\"symbols\":[";
  bool firstSymbolV2 = true;
  for (const auto& unit : result.units) {
    if (unit.ast.indexOnly) continue;
    for (const auto& declaration : unit.ast.declarations) {
      if (!firstSymbolV2) output << ',';
      firstSymbolV2 = false;
      output << "{\"unit\":\"" << jsonEscape(unit.ast.name) << "\",\"file\":\""
             << jsonEscape(unit.ast.file.generic_string()) << "\",\"name\":\"" << jsonEscape(declaration.name)
             << "\",\"qualified_name\":\"" << jsonEscape(unit.ast.name + "." + declaration.name)
             << "\",\"kind\":\"" << jsonEscape(declaration.kind) << "\",\"visibility\":\""
             << jsonEscape(declaration.visibility) << "\",\"type\":";
      if (declaration.type.empty()) output << "null"; else output << '"' << jsonEscape(declaration.type) << '"';
      output << ",\"line\":" << declaration.range.begin.line << '}';
    }
  }
  output << "],\"inheritance\":[";
  bool firstInheritanceV2 = true;
  for (const auto& unit : result.units) for (const auto& relation : unit.ast.inheritance) {
    if (!firstInheritanceV2) output << ',';
    firstInheritanceV2 = false;
    output << "{\"unit\":\"" << jsonEscape(unit.ast.name) << "\",\"type\":\"" << jsonEscape(relation.type)
           << "\",\"kind\":\"" << jsonEscape(relation.kind) << "\",\"base_type\":";
    if (relation.baseType.empty()) output << "null"; else output << '"' << jsonEscape(relation.baseType) << '"';
    output << '}';
  }
  output << "],\"dependencies\":[";
  for (std::size_t i = 0; i < result.graph.size(); ++i) {
    if (i) output << ',';
    output << "{\"from\":\"" << jsonEscape(result.graph[i].fromUnit) << "\",\"to\":\""
           << jsonEscape(result.graph[i].toUnit) << "\",\"section\":\""
           << (result.graph[i].section == UsesSection::Interface ? "interface" : "implementation")
           << "\",\"status\":\"" << toString(result.graph[i].status) << "\"}";
  }
  output << "],\"diagnostics\":[";
  bool firstDiagnosticV2 = true;
  for (const auto& diagnostic : result.diagnostics) {
    if (!firstDiagnosticV2) output << ',';
    firstDiagnosticV2 = false;
    const auto code = diagnostic.find("DPROJ") != std::string::npos || diagnostic.find("MSBuild") != std::string::npos
        ? "build_environment_incomplete" : "analysis_warning";
    output << "{\"code\":\"" << code << "\",\"severity\":\"warning\",\"message\":\""
           << jsonEscape(diagnostic) << "\"}";
  }
  for (const auto& unit : result.units) {
    if (unit.ast.indexOnly) continue;
    for (const auto& reference : unit.ast.references) {
      if (reference.status != ResolutionStatus::Unresolved) continue;
      if (!firstDiagnosticV2) output << ',';
      firstDiagnosticV2 = false;
      output << "{\"code\":\"unresolved_symbol\",\"severity\":\"warning\",\"message\":\"Symbol "
             << jsonEscape(reference.name) << " could not be resolved.\",\"file\":\""
             << jsonEscape(unit.ast.file.generic_string()) << "\",\"line\":" << reference.range.begin.line << '}';
    }
  }
  output << "],\"unit_mapping\":{\"available\":" << (result.unitMappingCatalogValid ? "true" : "false")
         << ",\"catalog\":";
  if (result.unitMappingCatalog.empty()) output << "null";
  else output << '"' << jsonEscape(result.unitMappingCatalog.generic_string()) << '"';
  output << ",\"mappings\":[";
  for (std::size_t i = 0; i < result.unitMappings.size(); ++i) {
    if (i) output << ',';
    const auto& mapping = result.unitMappings[i];
    output << "{\"source_unit\":\"" << jsonEscape(mapping.sourceUnit) << "\",\"target_unit\":";
    if (mapping.targetUnit.empty()) output << "null"; else output << '"' << jsonEscape(mapping.targetUnit) << '"';
    output << ",\"confidence\":\"" << jsonEscape(mapping.confidence)
           << "\",\"compatibility\":\"" << jsonEscape(mapping.compatibility)
           << "\",\"automatic_action\":\"" << jsonEscape(mapping.automaticAction) << "\"}";
  }
  output << "],\"diagnostics\":[";
  if (!result.unitMappingCatalogValid) output << "{\"code\":\""
      << (result.unitMappingCatalog.empty() ? "unit_mapping_catalog_not_provided" : "unit_mapping_catalog_invalid")
      << "\",\"severity\":\"info\",\"message\":\"Use compare-units with valid V500 and V600 indexes to generate verified mappings.\"}";
  output << "]},"
            "\"preprocessor\":{\"complete\":" << (result.preprocessorComplete ? "true" : "false")
         << ",\"configuration\":\"" << jsonEscape(result.configuration)
         << "\",\"platform\":\"" << jsonEscape(result.platform) << "\",\"defines\":[";
  for (std::size_t i = 0; i < result.defines.size(); ++i) {
    if (i) output << ',';
    output << '"' << jsonEscape(result.defines[i]) << '"';
  }
  output << "],\"includesResolved\":[";
  for (std::size_t i = 0; i < result.includesResolved.size(); ++i) {
    if (i) output << ',';
    output << '"' << jsonEscape(result.includesResolved[i].generic_string()) << '"';
  }
  output << "],\"inactiveRanges\":[";
  for (std::size_t i = 0; i < result.inactiveRanges.size(); ++i) {
    if (i) output << ',';
    const auto& range = result.inactiveRanges[i];
    output << "{\"file\":\"" << jsonEscape(range.file.generic_string()) << "\",\"startLine\":"
           << range.range.begin.line << ",\"endLine\":" << range.range.end.line
           << ",\"startOffset\":" << range.range.begin.offset << ",\"endOffset\":"
           << range.range.end.offset << ",\"condition\":\"" << jsonEscape(range.condition) << "\"}";
  }
  output << "]},\"recommendations\":{\"safeRemoveUses\":[";
  bool firstSafe = true;
  for (const auto& unit : result.units) {
    if (unit.ast.indexOnly) continue;
    std::error_code error;
    auto relative = std::filesystem::relative(unit.ast.file, sourceRoot, error);
    if (error) relative = unit.ast.file.filename();
    for (const auto& dependency : unit.dependencies) {
      const auto safe = dependency.status == DependencyStatus::Unused &&
                        dependency.confidence == Confidence::High &&
                        dependency.reasons.size() == 1 && dependency.reasons.front() == "no_references";
      if (!safe) continue;
      if (!firstSafe) output << ',';
      firstSafe = false;
      output << "{\"file\":\"" << jsonEscape(relative.generic_string()) << "\",\"unit\":\""
             << jsonEscape(dependency.unit) << "\",\"section\":\""
             << (dependency.section == UsesSection::Interface ? "interface" : "implementation")
             << "\",\"reason\":\"no_references\"}";
    }
  }
  output << "],\"blockedRemovals\":[";
  bool firstBlocked = true;
  for (const auto& unit : result.units) {
    if (unit.ast.indexOnly) continue;
    std::error_code error;
    auto relative = std::filesystem::relative(unit.ast.file, sourceRoot, error);
    if (error) relative = unit.ast.file.filename();
    for (const auto& dependency : unit.dependencies) {
      const auto safe = dependency.status == DependencyStatus::Unused &&
                        dependency.confidence == Confidence::High &&
                        dependency.reasons.size() == 1 && dependency.reasons.front() == "no_references";
      if (safe) continue;
      if (!firstBlocked) output << ',';
      firstBlocked = false;
      output << "{\"file\":\"" << jsonEscape(relative.generic_string()) << "\",\"unit\":\""
             << jsonEscape(dependency.unit) << "\",\"section\":\""
             << (dependency.section == UsesSection::Interface ? "interface" : "implementation")
             << "\",\"status\":\"" << toString(dependency.status) << "\",\"reasons\":[";
      bool firstReason = true;
      if (!dependency.references.empty()) {
        output << "\"references: ";
        for (std::size_t r = 0; r < dependency.references.size(); ++r) {
          if (r) output << ", ";
          output << jsonEscape(dependency.references[r].name);
        }
        output << '"';
        firstReason = false;
      }
      for (const auto& reason : dependency.reasons) {
        if (!firstReason) output << ',';
        firstReason = false;
        output << '"' << jsonEscape(reason) << '"';
      }
      output << "]}";
    }
  }
  output << "]},\"graph\":[";
  for (std::size_t i = 0; i < result.graph.size(); ++i) {
    if (i) output << ',';
    const auto& edge = result.graph[i];
    output << "{\"from\":\"" << jsonEscape(edge.fromUnit) << "\",\"to\":\""
           << jsonEscape(edge.toUnit) << "\",\"section\":\""
           << (edge.section == UsesSection::Interface ? "interface" : "implementation")
           << "\",\"status\":\"" << toString(edge.status) << "\"}";
  }
  output << "],\"cycles\":[";
  for (std::size_t i = 0; i < result.cycles.size(); ++i) {
    if (i) output << ',';
    output << '[';
    for (std::size_t n = 0; n < result.cycles[i].size(); ++n) {
      if (n) output << ',';
      output << '"' << jsonEscape(result.cycles[i][n]) << '"';
    }
    output << ']';
  }
  output << "]}";
  return output.str();
}

}  // namespace jdelphiast
