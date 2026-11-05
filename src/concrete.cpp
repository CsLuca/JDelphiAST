#include "jdelphiast/analyzer.hpp"
#include <cctype>
#include <stdexcept>

namespace jdelphiast {
std::string jsonSourceBytes(std::string_view source) {
  bool utf8 = true;
  for (std::size_t n = 0; n < source.size();) {
    const auto c = static_cast<unsigned char>(source[n]);
    if (c < 0x80) { ++n; continue; }
    const auto count = c >= 0xC2 && c <= 0xDF ? 2u : c >= 0xE0 && c <= 0xEF ? 3u : c >= 0xF0 && c <= 0xF4 ? 4u : 0u;
    if (!count || n + count > source.size()) { utf8 = false; break; }
    for (unsigned j = 1; j < count; ++j)
      if ((static_cast<unsigned char>(source[n+j]) & 0xC0) != 0x80) utf8 = false;
    const auto second = static_cast<unsigned char>(source[n+1]);
    if ((c == 0xE0 && second < 0xA0) || (c == 0xED && second >= 0xA0) ||
        (c == 0xF0 && second < 0x90) || (c == 0xF4 && second >= 0x90)) utf8 = false;
    if (!utf8) break;
    n += count;
  }
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (unsigned char c : source) {
    if (c == '\\') result += "\\\\";
    else if (c == '"') result += "\\\"";
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else if (c < 0x20 || (!utf8 && c >= 0x80)) {
      result += "\\u00"; result += digits[c >> 4]; result += digits[c & 15];
    } else result += static_cast<char>(c);
  }
  return result;
}
ConcreteSyntax tokenizeConcrete(std::string_view source) {
  ConcreteSyntax result;
  SourcePosition position;
  auto advance = [&]() {
    if (source[position.offset++] == '\n') { ++position.line; position.column = 1; }
    else ++position.column;
  };
  while (position.offset < source.size()) {
    const auto start = position;
    const auto at = start.offset;
    std::string kind = "symbol";
    bool terminated = true;
    if (at == 0 && source.starts_with("\xEF\xBB\xBF")) {
      kind = "bom"; advance(); advance(); advance();
      position.column = 1;
    } else if (std::isspace(static_cast<unsigned char>(source[at]))) {
      kind = "whitespace";
      while (position.offset < source.size() && std::isspace(static_cast<unsigned char>(source[position.offset]))) advance();
    } else if (source.substr(at, 2) == "//") {
      kind = "comment";
      while (position.offset < source.size() && source[position.offset] != '\n') advance();
    } else if (source[at] == '{' || source.substr(at, 2) == "(*") {
      const bool brace = source[at] == '{';
      const auto prefix = brace ? 1u : 2u;
      kind = at + prefix < source.size() && source[at + prefix] == '$' ? "directive" : "comment";
      for (unsigned n = 0; n < prefix; ++n) advance();
      const auto close = brace ? "}" : "*)";
      while (position.offset < source.size() && source.substr(position.offset, brace ? 1 : 2) != close) advance();
      terminated = position.offset < source.size();
      if (terminated) for (unsigned n = 0; n < (brace ? 1u : 2u); ++n) advance();
    } else if (source[at] == '\'') {
      kind = "string";
      std::size_t quoteCount = 0;
      while (at + quoteCount < source.size() && source[at + quoteCount] == '\'') ++quoteCount;
      const bool multiline = quoteCount >= 3 && quoteCount % 2 == 1 && at + quoteCount < source.size() &&
          (source[at + quoteCount] == '\r' || source[at + quoteCount] == '\n');
      if (multiline) {
        for (std::size_t n = 0; n < quoteCount; ++n) advance();
        const auto closing = source.find(std::string(quoteCount, '\''), position.offset);
        terminated = closing != std::string_view::npos;
        const auto end = terminated ? closing + quoteCount : source.size();
        while (position.offset < end) advance();
      } else {
      advance(); terminated = false;
      while (position.offset < source.size()) {
        if (source[position.offset] == '\'') {
          advance();
          if (position.offset < source.size() && source[position.offset] == '\'') advance();
          else { terminated = true; break; }
        } else if (source[position.offset] == '\r' || source[position.offset] == '\n') break;
        else advance();
      }
      }
    } else if (std::isalpha(static_cast<unsigned char>(source[at])) || source[at] == '_') {
      kind = "identifier";
      while (position.offset < source.size() && (std::isalnum(static_cast<unsigned char>(source[position.offset])) || source[position.offset] == '_')) advance();
    } else advance();
    result.tokens.push_back({kind, std::string(source.substr(at, position.offset - at)), {start, position}});
    if (!terminated) {
      result.complete = false;
      result.diagnostics.push_back("unterminated_" + kind + ":line=" + std::to_string(start.line) + ":byte=" + std::to_string(at));
    }
  }
  return result;
}

std::string writeConcrete(const ConcreteSyntax& syntax) {
  std::string result;
  for (const auto& token : syntax.tokens) {
    if (token.range.begin.offset != result.size() ||
        token.range.end.offset - token.range.begin.offset != token.bytes.size())
      throw std::invalid_argument("Concrete token ranges are not contiguous original-byte spans");
    result += token.bytes;
  }
  return result;
}
}
