#pragma once
#include "core/core.hpp"
#include "html_entities.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string_view>

namespace reaweb {
namespace app_names {
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }

inline bool ascii_letter(char c) { return lower(c) >= 'a' && lower(c) <= 'z'; }
inline bool ascii_digit(char c) { return c >= '0' && c <= '9'; }

inline void append_code_point(std::string& out, unsigned code) {
  if (code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) code = 0xfffd;
  if (code >= 0x80 && code < 0xa0) code = c1_references[code - 0x80];
  if (code < 0x80) { out += static_cast<char>(code); return; }
  const int bytes = code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
  out += static_cast<char>((0xff << (8 - bytes)) | (code >> (6 * (bytes - 1))));
  for (int shift = 6 * (bytes - 2); shift >= 0; shift -= 6)
    out += static_cast<char>(0x80 | ((code >> shift) & 63));
}

// Input starts immediately after '&' in title text (HTML RCDATA).
inline size_t decode_reference(std::string_view input, std::string& out) {
  if (!input.empty() && input.front() == '#') {
    const bool hex = input.size() > 1 && lower(input[1]) == 'x';
    const auto first = input.data() + (hex ? 2 : 1);
    unsigned code = 0;
    const auto parsed = std::from_chars(first, input.data() + input.size(), code, hex ? 16 : 10);
    if (parsed.ptr == first) return 0;
    append_code_point(out, parsed.ec == std::errc::result_out_of_range ? 0xfffd : code);
    const auto count = static_cast<size_t>(parsed.ptr - input.data());
    return count + (count < input.size() && input[count] == ';');
  }

  size_t n = 0;
  while (n < input.size() && n < longest_reference && (ascii_letter(input[n]) || ascii_digit(input[n]))) ++n;
  if (n < input.size() && input[n] == ';') ++n;
  for (; n; --n) {
    const auto key = input.substr(0, n);
    const auto end = std::end(named_references);
    const auto it = std::lower_bound(std::begin(named_references), end, key,
        [](const NamedReference& ref, std::string_view name) { return ref.name < name; });
    if (it != end && it->name == key) {
      out += it->value;
      return n;
    }
  }
  return 0;
}

inline std::string title_text(std::string_view text) {
  std::string decoded;
  for (size_t i = 0; i < text.size();) {
    const char c = text[i++];
    if (c == '&') {
      const auto consumed = decode_reference(text.substr(i), decoded);
      if (consumed) { i += consumed; continue; }
    }
    decoded += c;
  }
  std::string result;
  bool space = false;
  for (const char c : decoded) {
    if (std::string_view(" \t\r\n\f").find(c) != std::string_view::npos) { space = !result.empty(); continue; }
    if (space) { result += ' '; space = false; }
    result += c;
  }
  if (result.size() > 1024 || result.find('\0') != std::string::npos) return {};
  try { Json(result).dump(); } catch (const Json::type_error&) { return {}; }
  return result;
}

inline size_t tag_end(std::string_view html, size_t pos) {
  char quote = 0;
  for (; pos < html.size(); ++pos) {
    const char c = html[pos];
    if (quote) { if (c == quote) quote = 0; }
    else if (c == '"' || c == '\'') quote = c;
    else if (c == '>') return pos;
  }
  return std::string_view::npos;
}

// Read only the document header. Template contents do not belong to the document.
inline std::string html_title(const fs::path& entry) {
  std::ifstream input(entry, std::ios::binary);
  std::string html(65536, '\0');
  input.read(html.data(), static_cast<std::streamsize>(html.size()));
  html.resize(static_cast<size_t>(input.gcount()));
  auto folded = html;
  std::transform(folded.begin(), folded.end(), folded.begin(), lower);
  const std::string_view delimiters = " \t\r\n\f/>";
  unsigned template_depth = 0;
  size_t pos = 0;
  while ((pos = folded.find('<', pos)) != std::string::npos) {
    if (folded.compare(pos, 4, "<!--") == 0) {
      pos = folded.find("-->", pos + 4);
      if (pos == std::string::npos) break;
      pos += 3; continue;
    }
    ++pos;
    if (pos == folded.size()) break;
    const bool closing = folded[pos] == '/';
    if (closing) ++pos;
    if (pos == folded.size()) break;
    if (!ascii_letter(folded[pos]) && folded[pos] != '!' && folded[pos] != '?') continue;
    const auto end = folded.find_first_of(delimiters, pos);
    if (end == std::string::npos) break;
    const auto tag = folded.substr(pos, end - pos);
    pos = tag_end(folded, end);
    if (pos == std::string::npos) break;
    ++pos;
    if (tag == "template") {
      if (!closing) ++template_depth;
      else if (template_depth) --template_depth;
      continue;
    }
    if (!template_depth && ((closing && tag == "head") || (!closing && tag == "body"))) break;
    if (closing) continue;
    if (tag == "title" || tag == "script" || tag == "style" || tag == "noscript" ||
        tag == "textarea" || tag == "iframe" || tag == "xmp" || tag == "noembed" || tag == "noframes") {
      const auto closing_tag = "</" + tag;
      auto close = pos;
      do {
        close = folded.find(closing_tag, close);
        if (close == std::string::npos) return {};
        close += closing_tag.size();
      } while (close < folded.size() && delimiters.find(folded[close]) == std::string_view::npos);
      const auto end_tag = tag_end(folded, close);
      if (end_tag == std::string::npos) break;
      if (!template_depth && tag == "title")
        return title_text(std::string_view(html).substr(pos, close - closing_tag.size() - pos));
      pos = end_tag + 1;
    }
  }
  return {};
}

inline std::string slug(const std::string& name) {
  std::string out;
  bool separator = false;
  for (size_t i = 0; i < name.size();) {
    unsigned char c = name[i++];
    std::string part;
    if (c >= 0xc0) {
      unsigned code = c & (c < 0xe0 ? 31 : c < 0xf0 ? 15 : 7);
      while (i < name.size() && (static_cast<unsigned char>(name[i]) & 0xc0) == 0x80)
        code = (code << 6) | (name[i++] & 63);
      char digits[9];
      part = "u" + std::string(digits, std::to_chars(digits, digits + sizeof(digits), code, 16).ptr);
      separator = !out.empty();
    } else if ((lower(c) >= 'a' && lower(c) <= 'z') || (c >= '0' && c <= '9')) part = lower(c);
    else { separator = !out.empty(); continue; }
    if (out.size() + separator + part.size() > 40) break;
    if (separator) out += '-';
    out += part;
    separator = c >= 0xc0;
  }
  return out.empty() ? "untitled" : out;
}
} // namespace app_names
} // namespace reaweb
