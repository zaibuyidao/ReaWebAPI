#include "runtime/services.hpp"
#include <algorithm>

namespace reaweb {
Json drag_payload(const fs::path& base, const std::string& method, const Json& args) {
  if (args.size() != 1) throw Error("INVALID_ARGUMENT", "Expected one drag argument");
  if (method == "ReaWeb_DragText") {
    if (!args[0].is_string()) throw Error("INVALID_ARGUMENT", "Expected drag text");
    const auto text = args[0].get<std::string>();
    if (text.empty() || text.size() > value_limit || text.find('\0') != std::string::npos)
      throw Error("INVALID_ARGUMENT", "Drag text must be nonempty UTF-8 without NUL, at most 16 MiB");
    return {{"files", Json::array()}, {"text", text}};
  }
  if (!args[0].is_array() || args[0].empty() || args[0].size() > 256)
    throw Error("INVALID_ARGUMENT", "Expected 1..256 existing file paths");
  Json files = Json::array();
  for (const auto& value : args[0]) {
    auto path = existing_local_path(base, value);
    if (!fs::is_regular_file(path)) throw Error("INVALID_PATH", "Drag sources must be regular files");
    const auto text = path.u8string();
    if (std::find(files.begin(), files.end(), text) == files.end()) files.push_back(text);
  }
  return {{"files", files}, {"text", ""}};
}
}
