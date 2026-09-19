#include "runtime/services.hpp"
#include <cstdio>

namespace reaweb {
Json theme_colors(const Host& host) {
  auto get = host.native_function ? reinterpret_cast<int (*)(const char*, int)>(host.native_function("GetThemeColor")) : nullptr;
  auto convert = host.native_function ? reinterpret_cast<void (*)(int, int*, int*, int*)>(host.native_function("ColorFromNative")) : nullptr;
  Json colors, variables;
  const char* names[] = {"background", "text", "highlight", "panel", "border"};
  const char* keys[] = {"col_main_bg2", "col_main_text2", "col_seltrack2", "col_main_bg", "col_main_3dsh"};
  const char* fallback[] = {"#303030", "#eeeeee", "#487ca5", "#383838", "#202020"};
  bool available = get && convert;
  for (int i = 0; i < 5; ++i) {
    std::string css = fallback[i];
    const auto value = get && convert ? get(keys[i], 0) : -1;
    if (value != -1) {
      int r = 0, g = 0, b = 0; convert(value, &r, &g, &b);
      char text[8]; std::snprintf(text, sizeof(text), "#%02x%02x%02x", r & 255, g & 255, b & 255); css = text;
    }
    colors[names[i]] = css; variables[std::string("--reaper-") + names[i]] = css;
  }
  return {{"available", available}, {"colors", colors}, {"cssVariables", variables}};
}
}
