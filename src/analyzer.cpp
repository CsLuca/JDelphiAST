#include "jdelphiast/analyzer.hpp"
#include "jdelphiast/preprocessor.hpp"

#include <algorithm>
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
  explicit Lexer(std::string_view source) : source_(source) {}

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
  std::size_t i = 0;
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

  for (i = 0; tokens[i].kind != TokenKind::End; ++i) {
    if (word(tokens[i], "interface")) {
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

    if (inInterface && (word(tokens[i], "procedure") || word(tokens[i], "function") ||
                        word(tokens[i], "constructor") || word(tokens[i], "destructor"))) {
      if (tokens[i + 1].kind == TokenKind::Identifier) {
        ast.exports.push_back({tokens[i + 1].text, canonical(tokens[i].text), tokens[i + 1].range});
        ownNames.insert(canonical(tokens[i + 1].text));
        excluded.insert(i + 1);
      }
    }
    if (word(tokens[i], "procedure") || word(tokens[i], "function") || word(tokens[i], "constructor") ||
        word(tokens[i], "destructor")) {
      std::size_t n = i + 1;
      SourceRange nameRange;
      const auto name = qualifiedName(tokens, n, &nameRange);
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
              const auto parameterType = qualifiedName(tokens, typeIndex, &typeRange);
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
                variableTypes[canonical(parameter.name)] = parameter.type;
                lexicalNames.insert(canonical(parameter.name));
              }
              n = typeIndex - 1;
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
          if (word(tokens[directive], "deprecated")) declaration.deprecated = true;
          ++directive;
        }
        ast.declarations.push_back(std::move(declaration));
      }
    }
    if (inInterface && word(tokens[i], "type")) {
      std::size_t n = i + 1;
      while (tokens[n].kind != TokenKind::End && !word(tokens[n], "implementation") &&
             !word(tokens[n], "const") && !word(tokens[n], "var") && !word(tokens[n], "uses")) {
        if (tokens[n].kind == TokenKind::Identifier && tokens[n + 1].text == "=") {
          std::string kind = "type";
          if (word(tokens[n + 2], "class")) kind = "class";
          else if (word(tokens[n + 2], "interface")) kind = "interface";
          else if (word(tokens[n + 2], "record")) kind = "record";
          else if (tokens[n + 2].text == "(") kind = "enum";
          ast.exports.push_back({tokens[n].text, kind, tokens[n].range});
          ownNames.insert(canonical(tokens[n].text));
          excluded.insert(n);
        }
        ++n;
      }
    }
    if (inInterface && (word(tokens[i], "const") || word(tokens[i], "resourcestring"))) {
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
    if (inInterface && (word(tokens[i], "var") || word(tokens[i], "threadvar"))) {
      std::size_t n = i + 1;
      while (tokens[n].kind != TokenKind::End && !word(tokens[n], "implementation") &&
             !word(tokens[n], "type") && !word(tokens[n], "const") && !word(tokens[n], "uses") &&
             !word(tokens[n], "procedure") && !word(tokens[n], "function")) {
        if (tokens[n].kind == TokenKind::Identifier && tokens[n + 1].text == ":") {
          ast.exports.push_back({tokens[n].text, "variable", tokens[n].range});
          ownNames.insert(canonical(tokens[n].text));
          excluded.insert(n);
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
    ast.references.push_back({std::move(name), range, section, ResolutionStatus::Unresolved, {}, {}});
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
    if (tokens[i].kind != TokenKind::Identifier || tokens[i + 1].text != "=" ||
        !(word(tokens[i + 2], "class") || word(tokens[i + 2], "interface") || word(tokens[i + 2], "record"))) continue;
    const auto owner = tokens[i].text;
    std::string visibility = "private";
    int depth = 1;
    for (std::size_t n = i + 3; tokens[n].kind != TokenKind::End && depth > 0; ++n) {
      if (word(tokens[n], "class") || word(tokens[n], "record")) ++depth;
      else if (word(tokens[n], "end")) { --depth; continue; }
      if (depth != 1) continue;
      if (word(tokens[n], "private") || word(tokens[n], "protected") || word(tokens[n], "public") || word(tokens[n], "published")) {
        visibility = canonical(tokens[n].text);
        continue;
      }
      if (word(tokens[n], "procedure") || word(tokens[n], "function") || word(tokens[n], "constructor") || word(tokens[n], "destructor")) {
        if (tokens[n + 1].kind != TokenKind::Identifier) continue;
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
              std::size_t groupStart = cursor;
              while (groupStart > open + 1 && tokens[groupStart - 1].text != ";") --groupStart;
              std::string modifier;
              if (word(tokens[groupStart], "const") || word(tokens[groupStart], "var") || word(tokens[groupStart], "out")) {
                modifier = canonical(tokens[groupStart].text); ++groupStart;
              }
              for (std::size_t p = groupStart; p < cursor; ++p)
                if (tokens[p].kind == TokenKind::Identifier && !isKeyword(tokens[p].text))
                  declaration.parameters.push_back({tokens[p].text, parameterType,
                                                     {tokens[p].range.begin, typeRange.end}, modifier});
              cursor = typeCursor - 1;
            }
            ++cursor;
          }
        }
        if (word(tokens[n], "function") && tokens[cursor + 1].text == ":" && tokens[cursor + 2].kind == TokenKind::Identifier)
          declaration.type = tokens[cursor + 2].text;
        for (std::size_t d = cursor; tokens[d].kind != TokenKind::End && d < cursor + 16 && tokens[d].text != ";"; ++d) {
          if (word(tokens[d], "overload")) declaration.overload = true;
          if (word(tokens[d], "override")) declaration.isOverride = true;
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
        if (tokens[n + 2].text == ":" && tokens[n + 3].kind == TokenKind::Identifier) declaration.type = tokens[n + 3].text;
        ast.declarations.push_back(std::move(declaration));
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
      const auto left = qualifiedName(tokens, assignmentOperator, &leftRange);
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
  for (auto& declaration : ast.declarations) {
    if (declaration.isOverride || declaration.name.find('.') == std::string::npos) continue;
    const auto simple = declaration.name.substr(declaration.name.find_last_of('.') + 1);
    declaration.isOverride = std::any_of(ast.declarations.begin(), ast.declarations.end(), [&](const AstDeclaration& candidate) {
      return candidate.isOverride && canonical(candidate.name) == canonical(simple);
    });
  }
  return ast;
}

std::string jsonEscape(std::string_view value) {
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
                               std::vector<std::filesystem::path> includePaths) {
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
  const PreprocessorOptions preprocessorOptions{defines_, includePaths_, 64};
  for (const auto& input : inputs_) {
    const auto preprocessed = preprocess(input.path, input.source, preprocessorOptions);
    auto ast = parseSource(input.path, preprocessed.source);
    ast.sourceHash = sha256(input.source);
    ast.complete = ast.complete && preprocessed.complete;
    ast.includesResolved = preprocessed.includesResolved;
    ast.inactiveRanges = preprocessed.inactiveRanges;
    ast.preprocessorReasons = preprocessed.reasons;
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

  for (auto& analyzed : result.units) {
    if (analyzed.ast.indexOnly) continue;
    for (auto& call : analyzed.ast.calls) {
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
      output << ',';
      writePosition(output, declaration.range);
      output << ",\"parameters\":[";
      for (std::size_t p = 0; p < declaration.parameters.size(); ++p) {
        if (p) output << ',';
        const auto& parameter = declaration.parameters[p];
        output << "{\"name\":\"" << jsonEscape(parameter.name) << "\",\"type\":\""
               << jsonEscape(parameter.type) << "\",";
        writePosition(output, parameter.range);
        output << '}';
      }
      output << "]}";
    }
    output << "],\"calls\":[";
    for (std::size_t c = 0; c < unit.ast.calls.size(); ++c) {
      if (c) output << ',';
      const auto& call = unit.ast.calls[c];
      output << "{\"name\":\"" << jsonEscape(call.name) << "\",";
      writePosition(output, call.range);
      output << ",\"arguments\":[";
      for (std::size_t a = 0; a < call.arguments.size(); ++a) {
        if (a) output << ',';
        const auto& argument = call.arguments[a];
        output << "{\"kind\":\"" << jsonEscape(argument.kind) << "\",\"text\":\""
               << jsonEscape(argument.text) << "\"";
        if (!argument.resolvedType.empty())
          output << ",\"resolvedType\":\"" << jsonEscape(argument.resolvedType) << "\"";
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
      if (resolved) output << '"' << jsonEscape(reference.declaringUnit + "." + reference.name) << '"'; else output << "null";
      output << ",\"kind\":\"" << jsonEscape(resolvedKind) << "\",\"usage\":\"" << usage << "\",\"declared_in_unit\":";
      if (resolved) output << '"' << jsonEscape(reference.declaringUnit) << '"'; else output << "null";
      output << ",\"declared_in_file\":";
      if (owner == result.units.end() || owner->ast.file == "<index>") output << "null";
      else output << '"' << jsonEscape(owner->ast.file.generic_string()) << '"';
      output << ",\"resolved\":" << (resolved ? "true" : "false")
             << ",\"confidence\":\"" << (resolved ? "high" : "low") << "\",\"source_index\":\""
             << (resolved ? (owner != result.units.end() ? owner->ast.sourceIndex : "project") : "unknown") << "\"";
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
