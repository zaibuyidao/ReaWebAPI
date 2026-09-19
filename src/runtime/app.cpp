#include "runtime/services.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>

namespace reaweb {
Json app_info(const fs::path& root, const fs::path& data, const std::string& id) {
  Json result = {{"id", id}, {"name", root.filename().u8string()}, {"version", nullptr},
    {"rootPath", fs::canonical(root).u8string()}, {"dataPath", fs::absolute(data).lexically_normal().u8string()}};
  const auto path = root / "app.json";
  if (fs::exists(path)) {
    if (!fs::is_regular_file(path) || fs::file_size(path) > 65536) throw Error("APP_MANIFEST_INVALID", "app.json exceeds 64 KiB or is not a file");
    std::ifstream stream(path, std::ios::binary);
    auto manifest = Json::parse(stream, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) throw Error("APP_MANIFEST_INVALID", "app.json must contain a JSON object");
    for (const auto* field : {"name", "version"}) if (manifest.contains(field)) {
      if (!manifest[field].is_string()) throw Error("APP_MANIFEST_INVALID", std::string("Invalid App ") + field);
      const auto value = manifest[field].get<std::string>();
      if (value.empty() || value.find('\0') != std::string::npos ||
          (std::string(field) == "name" && (std::count_if(value.begin(), value.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }) > 256 || value.find_first_not_of(" \t\r\n") == std::string::npos)) ||
          (std::string(field) == "version" && !std::regex_match(value, std::regex("[0-9]+\\.[0-9]+\\.[0-9]+(?:-[A-Za-z0-9.-]+)?"))))
        throw Error("APP_MANIFEST_INVALID", std::string("Invalid App ") + field);
      result[field] = value;
    }
  }
  std::error_code error; fs::create_directories(data, error);
  if (error) throw Error("APP_DATA_UNAVAILABLE", "Cannot create the App data directory: " + error.message());
  result["dataPath"] = fs::canonical(data).u8string();
  // Verify write access even when a previous installation created this directory.
  fs::path probe;
  for (int attempt = 0; attempt < 4; ++attempt) {
    auto candidate = data / (".write-check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(attempt));
    if (fs::create_directory(candidate, error)) { probe = candidate; break; }
    if (error) break;
  }
  if (probe.empty()) throw Error("APP_DATA_UNAVAILABLE", "App data directory is not writable");
  struct RemoveProbe { fs::path path; ~RemoveProbe() { std::error_code ignored; fs::remove(path / "check", ignored); fs::remove(path, ignored); } } cleanup{probe};
  std::ofstream check(probe / "check", std::ios::binary); check << '1'; check.close();
  if (!check) throw Error("APP_DATA_UNAVAILABLE", "Cannot write to the App data directory");
  return result;
}
}
