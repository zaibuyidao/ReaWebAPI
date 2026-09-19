#include "host_io.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <set>
#ifdef _WIN32
#include <windows.h>
#endif

namespace reaweb {
namespace {
fs::path path_arg(const fs::path& base, const Json& arg) {
  if (!arg.is_string()) throw Error("INVALID_PATH", "Expected a filesystem path");
  const auto text = arg.get<std::string>();
  if (text.empty() || text.size() > 32768 || text.find('\0') != std::string::npos || text.find("://") != std::string::npos)
    throw Error("INVALID_PATH", "Expected a local path, without a URL scheme or NUL");
  auto path = fs::u8path(text);
  return fs::weakly_canonical(path.is_absolute() ? path : base / path);
}
Json options(const Json& args, size_t index, const std::set<std::string>& allowed) {
  if (args.size() <= index) return Json::object();
  const auto& value = args[index];
  if (!value.is_object()) throw Error("INVALID_ARGUMENT", "Expected file options");
  for (const auto& item : value.items()) if (!allowed.count(item.key())) throw Error("INVALID_ARGUMENT", "Unknown file option: " + item.key());
  return value;
}
void text_encoding(const std::string& text) {
  try { (void)Json(text).dump(); }
  catch (const Json::exception&) { throw Error("FILE_ENCODING", "File is not valid UTF-8; read it as binary"); }
}
std::string encoding(const Json& opt) {
  const auto name = opt.value("encoding", std::string("utf8"));
  if (name != "utf8" && name != "binary") throw Error("INVALID_ARGUMENT", "encoding must be utf8 or binary");
  return name;
}
Json stat(const fs::path& path) {
  const auto info = fs::status(path);
  return {{"path", path.u8string()}, {"exists", fs::exists(info)},
    {"type", fs::is_regular_file(info) ? "file" : fs::is_directory(info) ? "directory" : "other"},
    {"size", fs::is_regular_file(info) ? Json(fs::file_size(path)) : Json(nullptr)}};
}
}
bool is_file_method(const std::string& name) {
  return name == "ReaWeb_ReadFile" || name == "ReaWeb_WriteFile" || name == "ReaWeb_Stat" ||
    name == "ReaWeb_ReadDirectory" || name == "ReaWeb_MakeDirectory";
}
Json file_call(const fs::path& base, const std::string& method, const Json& args) {
  try {
    const bool write = method == "ReaWeb_WriteFile";
    if (!args.is_array() || args.size() < (write ? 2u : 1u) || args.size() > (write ? 3u : 2u))
      throw Error("INVALID_ARGUMENT", "Wrong file argument count");
    const auto path = path_arg(base, args[0]);
    const auto opt = options(args, write ? 2 : 1, write ? std::set<std::string>{"encoding", "overwrite"} :
      method == "ReaWeb_ReadFile" ? std::set<std::string>{"encoding"} :
      method == "ReaWeb_MakeDirectory" ? std::set<std::string>{"recursive"} : std::set<std::string>{});
    if (method == "ReaWeb_Stat") return stat(path);
    if (method == "ReaWeb_MakeDirectory") {
      const bool recursive = opt.value("recursive", false);
      return recursive ? fs::create_directories(path) : fs::create_directory(path);
    }
    if (method == "ReaWeb_ReadDirectory") {
      if (!fs::is_directory(path)) throw Error("FILE_NOT_DIRECTORY", "Path is not a directory");
      std::vector<fs::path> names;
      for (const auto& entry : fs::directory_iterator(path)) {
        if (names.size() >= 4096) throw Error("DIRECTORY_LIMIT", "Directory exceeds 4096 entries");
        names.push_back(entry.path());
      }
      std::sort(names.begin(), names.end());
      auto result = Json::array();
      for (const auto& name : names) {
        auto info = stat(name); info["name"] = name.filename().u8string(); result.push_back(std::move(info));
      }
      return result;
    }
    const bool binary = encoding(opt) == "binary";
    if (method == "ReaWeb_ReadFile") {
      if (!fs::is_regular_file(path)) throw Error("FILE_NOT_FOUND", "Path is not a regular file");
      if (fs::file_size(path) > value_limit) throw Error("BUFFER_LIMIT", "File exceeds 16 MiB");
      std::ifstream stream(path, std::ios::binary);
      if (!stream) throw Error("FILE_IO", "Cannot open file");
      std::string data; char block[8192];
      while (stream) {
        stream.read(block, sizeof(block));
        if (data.size() + static_cast<size_t>(stream.gcount()) > value_limit) throw Error("BUFFER_LIMIT", "File exceeds 16 MiB");
        data.append(block, static_cast<size_t>(stream.gcount()));
      }
      if (!stream.eof()) throw Error("FILE_IO", "Cannot read file");
      if (binary) return encode_binary(data.data(), data.size());
      text_encoding(data);
      return data;
    }
    if (!write) throw Error("UNKNOWN_API", "Unknown file operation");
    if (!binary && !args[1].is_string()) throw Error("INVALID_ARGUMENT", "UTF-8 writes require a string");
    const auto data = binary ? decode_binary(args[1]) : args[1].get<std::string>();
    if (data.size() > value_limit) throw Error("BUFFER_LIMIT", "File exceeds 16 MiB");
    if (!binary) text_encoding(data);
    const bool overwrite = opt.value("overwrite", false);
    if (!overwrite && fs::exists(path)) throw Error("FILE_EXISTS", "File exists; pass overwrite: true to replace it");
    static std::atomic<unsigned> counter{0};
    auto temporary = path;
    temporary += ".reaweb-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++counter) + ".tmp";
    try {
      {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(data.data(), static_cast<std::streamsize>(data.size())); stream.flush();
        if (!stream) throw Error("FILE_IO", "Cannot write file");
      }
#ifdef _WIN32
      if (!MoveFileExW(temporary.c_str(), path.c_str(), (overwrite ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
        throw Error("FILE_IO", "Cannot commit file (destination may exist or be in use)");
#else
      if (overwrite) fs::rename(temporary, path);
      else { fs::create_hard_link(temporary, path); fs::remove(temporary); }
#endif
    } catch (...) { std::error_code ignored; fs::remove(temporary, ignored); throw; }
    return {{"path", path.u8string()}, {"bytes", data.size()}};
  } catch (const fs::filesystem_error& e) { throw Error("FILE_IO", e.what()); }
  catch (const Json::exception&) { throw Error("INVALID_ARGUMENT", "Invalid file option type"); }
}
}
