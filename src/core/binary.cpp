#include "core/core.hpp"
#include <cstring>

namespace reaweb {
static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
Json encode_binary(const char* p, size_t length) {
  if (length > value_limit) throw Error("BUFFER_LIMIT", "Binary result exceeds 16 MiB");
  std::string out;
  out.reserve((length + 2) / 3 * 4);
  for (size_t i = 0; i < length; i += 3) {
    const auto a = static_cast<unsigned char>(p[i]);
    const auto b = i + 1 < length ? static_cast<unsigned char>(p[i+1]) : 0;
    const auto c = i + 2 < length ? static_cast<unsigned char>(p[i+2]) : 0;
    out += alphabet[a >> 2]; out += alphabet[((a & 3) << 4) | (b >> 4)];
    out += i + 1 < length ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
    out += i + 2 < length ? alphabet[c & 63] : '=';
  }
  return Json{{"__reawebBytes", std::move(out)}};
}
std::string decode_binary(const Json& v) {
  if (v.is_string()) {
    const auto& text = v.get_ref<const std::string&>();
    if (text.size() > value_limit) throw Error("BUFFER_LIMIT", "String exceeds 16 MiB");
    return text;
  }
  if (!v.is_object() || v.size() != 1 || !v.contains("__reawebBytes") || !v["__reawebBytes"].is_string())
    throw Error("INVALID_ARGUMENT", "Expected a Uint8Array or string");
  const auto& s = v["__reawebBytes"].get_ref<const std::string&>();
  if (s.size() % 4 || s.size() > (value_limit + 2) / 3 * 4)
    throw Error("BUFFER_LIMIT", "Invalid or oversized binary argument");
  std::string out;
  out.reserve(s.size() / 4 * 3);
  for (size_t i = 0; i < s.size(); i += 4) {
    unsigned int n = 0;
    int padding = 0;
    for (int j = 0; j < 4; ++j) {
      if (s[i+j] == '=') {
        if (i + 4 != s.size() || j < 2) throw Error("INVALID_ARGUMENT", "Invalid base64 padding");
        ++padding; n <<= 6;
      } else {
        const auto q = std::strchr(alphabet, s[i+j]);
        if (padding || s[i+j] == 0 || !q) throw Error("INVALID_ARGUMENT", "Invalid base64 data");
        n = (n << 6) | static_cast<unsigned int>(q - alphabet);
      }
    }
    out += static_cast<char>((n >> 16) & 255);
    if (padding < 2) out += static_cast<char>((n >> 8) & 255);
    if (!padding) out += static_cast<char>(n & 255);
  }
  return out;
}
}
