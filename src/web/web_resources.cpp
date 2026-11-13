#include <httplib.h>
#include "web/web_resources.hpp"
#include "web/browser_probe.hpp"
#include "core/worker.hpp"
#include "core/file_time.hpp"
#include "runtime/external_settings.hpp"
#include <fstream>
#include <random>
#include <atomic>
#include <set>
#ifndef _WIN32
#include <sys/file.h>
#endif

namespace reaweb {
namespace {
bool valid_id(const std::string& id) {
  return !id.empty() && id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;
}
std::string root_identity(const fs::path& root) {
  auto value = fs::canonical(root).generic_u8string();
#ifdef _WIN32
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c);
  });
#endif
  return value;
}
std::string encode_path(const std::string& path) {
  const char* hex = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : path) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~' || c == '/') result += static_cast<char>(c);
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}
bool contained(const fs::path& root, const fs::path& path) {
  const auto relative = path.lexically_relative(root);
  return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
std::string decode_path(const std::string& value) {
  std::string result;
  auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
  for (size_t i = 0; i < value.size(); ++i) {
    unsigned char c = value[i];
    if (c == '%') {
      if (i + 2 >= value.size() || hex(value[i + 1]) < 0 || hex(value[i + 2]) < 0)
        throw Error("INVALID_PATH", "Malformed resource escape");
      c = static_cast<unsigned char>(hex(value[i + 1]) * 16 + hex(value[i + 2])); i += 2;
    }
    if (c < 32 || c == 127 || c == '\\' || c == ':') throw Error("INVALID_PATH", "Invalid resource path");
    result += static_cast<char>(c);
  }
  return result;
}
Json read_record(const fs::path& path) {
  if (!fs::is_regular_file(path) || fs::file_size(path) > 65536)
    throw std::runtime_error("Identity record exceeds limit or is not a file");
  std::ifstream input(path, std::ios::binary);
  return Json::parse(input);
}
// Serialize root binding across REAPER processes without stale lock ownership.
struct IdentityLock {
#ifdef _WIN32
  HANDLE file = INVALID_HANDLE_VALUE;
  OVERLAPPED overlapped{};
  explicit IdentityLock(const fs::path& path, bool exclusive = true, bool wait = true) {
    file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw Error("APP_DATA_UNAVAILABLE", "Cannot lock App identity");
    if (!LockFileEx(file, (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY), 0, 1, 0, &overlapped)) {
      const auto error = GetLastError();
      CloseHandle(file);
      if (!wait && error == ERROR_LOCK_VIOLATION)
        throw Error("APP_ID_CONFLICT", "Old binding has active or initializing windows");
      throw Error("APP_DATA_UNAVAILABLE", "Cannot lock App identity");
    }
  }
  ~IdentityLock() { UnlockFileEx(file, 0, 1, 0, &overlapped); CloseHandle(file); }
#else
  int file = -1;
  explicit IdentityLock(const fs::path& path, bool exclusive = true, bool wait = true) {
    file = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (file < 0) throw Error("APP_DATA_UNAVAILABLE", "Cannot lock App identity");
    if (flock(file, (exclusive ? LOCK_EX : LOCK_SH) | (wait ? 0 : LOCK_NB)) != 0) {
      const auto error = errno;
      ::close(file);
      if (!wait && (error == EWOULDBLOCK || error == EAGAIN))
        throw Error("APP_ID_CONFLICT", "Old binding has active or initializing windows");
      throw Error("APP_DATA_UNAVAILABLE", "Cannot lock App identity");
    }
  }
  ~IdentityLock() { flock(file, LOCK_UN); ::close(file); }
#endif
};

bool same_binding_path(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
  return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
  return left == right;
#endif
}
bool binding_contains(const fs::path& root, const fs::path& path) {
  auto child = path.begin();
  for (const auto& part : root) {
    if (child == path.end() || !same_binding_path(part, *child++)) return false;
  }
  return child != path.end();
}
fs::path binding_entry(const Json& value) {
  if (!value.is_string()) throw std::runtime_error("Identity entry must be a relative HTML path");
  const auto text = value.get<std::string>();
  const auto path = fs::u8path(text);
  auto extension = path.extension().u8string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (text.empty() || text.find_first_of(":\\") != std::string::npos || text.find('\0') != std::string::npos ||
      path.has_root_path() || (extension != ".html" && extension != ".htm"))
    throw std::runtime_error("Identity entry must be a relative HTML path");
  for (const auto& part : path) if (part == "..") throw std::runtime_error("Identity entry escapes App root");
  return path.lexically_normal();
}
fs::file_status binding_status(const fs::path& path) {
  std::error_code error;
  const auto status = fs::status(path, error);
  if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory)
    throw fs::filesystem_error("Cannot confirm path status", path, error);
  if (!fs::exists(status)) {
    // A missing drive/share must not be mistaken for a removed installation.
    const auto anchor = fs::status(path.root_path(), error);
    if (error || !fs::is_directory(anchor))
      throw fs::filesystem_error("Cannot confirm volume status", path.root_path(),
        error ? error : std::make_error_code(std::errc::no_such_device));
  }
  return status;
}
bool binding_entry_valid(const fs::path& root, const fs::path& entry) {
  auto target = root;
  auto status = binding_status(root);
  for (const auto& part : entry) {
    if (part == ".") continue;
    if (!fs::is_directory(status)) return false;
    target /= part;
    status = binding_status(target);
    if (!fs::exists(status)) {
      // Do not accept a dangling link to an external path as a missing local file.
      if (fs::is_symlink(fs::symlink_status(target))) {
        auto link = fs::read_symlink(target);
        if (link.is_relative()) link = target.parent_path() / link;
        if (!binding_contains(root, fs::weakly_canonical(link)))
          throw Error("APP_ORIGIN_INVALID", "Identity entry escapes App root");
      }
      return false;
    }
    target = fs::canonical(target);
    if (!binding_contains(root, target)) throw Error("APP_ORIGIN_INVALID", "Identity entry escapes App root");
  }
  return fs::is_regular_file(status);
}

void write_identity_record(const fs::path& record, const Json& data, const char* code) {
  auto temporary = record; temporary += "." + external_token().substr(0, 16) + ".tmp";
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << data.dump(2) << '\n';
    output.close();
    if (!output) throw std::runtime_error("Cannot persist identity");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), record.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Cannot replace identity record");
#else
    fs::rename(temporary, record);
#endif
  } catch (const std::exception& error) {
    std::error_code ignored; fs::remove(temporary, ignored);
    throw Error(code, record.u8string() + ": " + error.what());
  }
}
std::string read_auto_identity(const fs::path& record) {
  try {
    const auto status = fs::symlink_status(record);
    if (!fs::exists(status)) return {};
    if (!fs::is_regular_file(status) || fs::file_size(record) > 65536)
      throw std::runtime_error("Identity marker must be a regular file no larger than 64 KiB");
    std::ifstream input(record, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read identity marker");
    std::set<std::string> keys;
    const auto data = Json::parse(input, [&](int, Json::parse_event_t event, Json& value) {
      if (event == Json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
        throw std::runtime_error("Duplicate identity marker field");
      return true;
    });
    if (!data.is_object() || data.size() != 2 || !data.at("schema").is_number_integer() ||
        data["schema"] != 1 || !data.at("appId").is_string())
      throw std::runtime_error("Invalid identity marker schema");
    const auto id = data["appId"].get<std::string>();
    if (id.size() != 36 || id.compare(0, 4, "app-") != 0 || id.substr(4).find_first_not_of("0123456789abcdef") != std::string::npos)
      throw std::runtime_error("Invalid automatic App ID");
    return id;
  } catch (const std::exception& error) {
    throw Error("APP_IDENTITY_INVALID", record.u8string() + ": " + error.what());
  }
}
}

std::string app_identity(const fs::path& root) {
  const auto manifest = root / "app.json";
  if (fs::exists(manifest)) {
    Json value;
    try { value = read_record(manifest); }
    catch (const std::exception& error) { throw Error("APP_MANIFEST_INVALID", error.what()); }
    if (!value.is_object()) throw Error("APP_MANIFEST_INVALID", "app.json must contain a JSON object");
    if (value.contains("id")) {
      if (!value["id"].is_string() || !valid_id(value["id"].get<std::string>()))
        throw Error("APP_MANIFEST_INVALID", "app.json.id must contain only lowercase a-z, 0-9 and -");
      return value["id"].get<std::string>();
    }
  }
  return {};
}

AppBinding open_app_binding(const fs::path& root, const fs::path& apps, const fs::path& entry,
    const std::string& fallback_id, const std::function<void(const std::string&)>& prepare) {
  const auto canonical_root = fs::canonical(root);
  const auto html = resolve_html(canonical_root, entry.u8string());
  if (!binding_contains(canonical_root, html)) throw Error("INVALID_PATH", "HTML entry escapes App root");
  auto id = app_identity(canonical_root);
  if (id.empty()) id = fallback_id;
  if (!id.empty()) {
    if (prepare) prepare(id);
    return {id, bind_app_identity(canonical_root, apps / id, id, html)};
  }
  const auto directory = canonical_root / ".reawebapi";
  const auto marker = directory / "identity.json";
  std::unique_ptr<IdentityLock> lock;
  try {
    fs::create_directory(directory);
    const auto status = fs::symlink_status(directory);
    if (!fs::is_directory(status) || !same_binding_path(fs::canonical(directory), directory))
      throw std::runtime_error("Identity directory must be local to this App root");
    const auto lock_path = directory / ".identity.lock";
    const auto lock_status = fs::symlink_status(lock_path);
    if (fs::exists(lock_status) && !fs::is_regular_file(lock_status))
      throw std::runtime_error("Invalid identity lock file");
    // Lock order is always Root marker first, then the existing App ID binding lock.
    lock = std::make_unique<IdentityLock>(lock_path);
  } catch (const std::exception& error) {
    throw Error("APP_IDENTITY_UNAVAILABLE", "Cannot persist App identity at " + marker.u8string() + ": " + error.what());
  }
  id = read_auto_identity(marker);
  bool save = id.empty();
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (save) id = "app-" + external_token().substr(0, 32);
    if (prepare) prepare(id);
    // Persist before binding/navigation. A retry after interruption uses the same ID.
    if (save) write_identity_record(marker, {{"schema", 1}, {"appId", id}}, "APP_IDENTITY_UNAVAILABLE");
    bool duplicate = false;
    try { return {id, bind_app_identity(canonical_root, apps / id, id, html, &duplicate)}; }
    catch (const Error& error) {
      if (error.code != "APP_ID_CONFLICT" || !duplicate) throw;
      save = true;
    }
  }
  throw Error("APP_IDENTITY_UNAVAILABLE", "Cannot allocate an unbound App ID: " + marker.u8string());
}

std::shared_ptr<void> bind_app_identity(const fs::path& root, const fs::path& profile,
    const std::string& app_id, const fs::path& entry, bool* valid_root_conflict) {
  if (valid_root_conflict) *valid_root_conflict = false;
  if (!valid_id(app_id)) throw Error("APP_MANIFEST_INVALID", "Invalid App ID");
  const auto canonical_root = fs::canonical(root);
  const auto html = resolve_html(canonical_root, entry.u8string());
  if (!binding_contains(canonical_root, html)) throw Error("INVALID_PATH", "HTML entry escapes App root");
  const auto relative_entry = fs::relative(html, canonical_root).generic_u8string();
  fs::create_directories(profile);
  IdentityLock lock(profile / ".identity.lock");
  const auto record = profile / "origin.json";
  const auto active = profile / ".identity.active";
  const auto identity = root_identity(root);
  Json data{{"schema", 2}, {"root", identity}, {"appId", app_id}, {"origin", "reaweb://" + app_id}};
  std::shared_ptr<IdentityLock> replacement;
  try {
    // Inspect the directory entry too, so a dangling metadata link is not overwritten.
    if (fs::exists(fs::symlink_status(record))) {
      data = read_record(record);
      if (!data.at("schema").is_number_integer() || (data["schema"] != 1 && data["schema"] != 2))
        throw std::runtime_error("Invalid identity schema");
      const int schema = data.at("schema").get<int>();
      if ((schema != 1 && schema != 2) || !data.at("root").is_string() ||
          (schema == 2 && (data.at("appId") != app_id || data.at("origin") != "reaweb://" + app_id)))
        throw std::runtime_error("Identity record does not match this App");
      const auto previous = data.at("root").get<std::string>();
      const auto old_root = fs::u8path(previous);
      if (!old_root.is_absolute() || previous.find('\0') != std::string::npos)
        throw std::runtime_error("Identity root must be an absolute path");
      auto old_entry = data.contains("entry") ? binding_entry(data["entry"]) : fs::path();
      std::string state = "old Root and original entry status unconfirmed";
      const auto conflict = [&](const std::string& reason) {
        return Error("APP_ID_CONFLICT", "APP_ID_CONFLICT: App ID '" + app_id + "', bound Root '" + previous +
          "', requested Root '" + identity + "', " + state + ". " + reason + ". Identity record: " +
          fs::absolute(record).u8string() + ". After confirming the old installation is no longer in use, remove its directory and retry.");
      };
      bool same = false;
      try {
        const auto status = binding_status(old_root);
        const bool exists = fs::exists(status);
        state = exists ? "old Root exists, original entry status unconfirmed" : "old Root is missing, original entry is unavailable";
        const auto normalized = exists ? fs::canonical(old_root) : old_root.lexically_normal();
        same = exists && (same_binding_path(normalized, canonical_root) || fs::equivalent(old_root, canonical_root));
        if (!same && exists && old_entry.empty()) {
          const auto manifest_path = normalized / "app.json";
          if (fs::exists(binding_status(manifest_path))) {
            if (!binding_contains(normalized, fs::canonical(manifest_path)))
              throw conflict("Cannot confirm original entry: manifest escapes old Root");
            try {
              const auto manifest = read_record(manifest_path);
              if (!manifest.is_object() || (manifest.contains("id") && manifest["id"] != app_id) ||
                  (manifest.contains("schemaVersion") && (!manifest["schemaVersion"].is_number_integer() || manifest["schemaVersion"] != 1)))
                throw std::runtime_error("Manifest identity or schema does not match");
              if (manifest.contains("entry")) old_entry = binding_entry(manifest["entry"]);
            } catch (const fs::filesystem_error&) { throw; }
            catch (const std::exception& error) { throw conflict(std::string("Cannot confirm original entry from app.json: ") + error.what()); }
          }
        }
        bool entry_valid = false;
        if (exists && !old_entry.empty()) {
          entry_valid = binding_entry_valid(normalized, old_entry);
          state = entry_valid ? "old Root exists, original entry is valid" : "old Root exists, original entry is missing or not a regular file";
        }
        if (!same) {
          if (exists && old_entry.empty())
            throw conflict("Missing entry metadata, cannot safely determine the original entry. app.json does not define an authoritative entry");
          if (exists && entry_valid) {
            if (valid_root_conflict) *valid_root_conflict = true;
            throw conflict("The old binding is still valid");
          }
          try { replacement = std::make_shared<IdentityLock>(active, true, false); }
          catch (const Error& error) {
            if (error.code == "APP_ID_CONFLICT") throw conflict(error.what());
            throw;
          }
        }
      } catch (const fs::filesystem_error& error) { throw conflict(std::string("Cannot confirm old Root or entry status: ") + error.what()); }
      if (schema == 1) {
        data["schema"] = 2; data["appId"] = app_id; data["origin"] = "reaweb://" + app_id; data.erase("port");
      }
      // Keep the original entry when opening another document in the same App.
      if (same && schema == 2 && !old_entry.empty()) return std::make_shared<IdentityLock>(active, false);
    }
  } catch (const Error&) { throw; }
  catch (const std::exception& error) { throw Error("APP_ORIGIN_INVALID", error.what()); }
  data["root"] = identity;
  data["entry"] = relative_entry;
  write_identity_record(record, data, "APP_DATA_UNAVAILABLE");
  replacement.reset();
  return std::make_shared<IdentityLock>(active, false);
}

WebResources::WebResources(const fs::path& root, const std::string& app_id, bool isolation_required)
  : root_(fs::canonical(root)), origin_("reaweb://" + app_id), isolation_required_(isolation_required) {
  if (!valid_id(app_id)) throw Error("APP_MANIFEST_INVALID", "Invalid App ID");
}
WebResources::~WebResources() { clear_media(); }

struct WebResources::Media final : ResourceReader {
  fs::path path;
#ifdef _WIN32
  HANDLE file = INVALID_HANDLE_VALUE;
#else
  std::ifstream file;
#endif
  uint64_t length;
  fs::file_time_type modified;
  std::string type;
  std::mutex mutex;
  std::atomic<bool> active{true};
  Media(const fs::path& source, std::string mime) : path(source),
#ifndef _WIN32
    file(source, std::ios::binary),
#endif
    length(fs::file_size(source)), modified(fs::last_write_time(source)), type(std::move(mime)) {
    if (length > 64ull * 1024 * 1024 * 1024) throw Error("MEDIA_LIMIT", "Media file exceeds 64 GiB");
#ifdef _WIN32
    file = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw Error("FILE_NOT_FOUND", "Cannot open media file");
#else
    if (!file) throw Error("FILE_NOT_FOUND", "Cannot open media file");
#endif
  }
  ~Media() override {
#ifdef _WIN32
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
#endif
  }
  bool valid() {
    std::error_code error;
    if (!active || !fs::is_regular_file(path, error) || error || fs::file_size(path, error) != length || error ||
        fs::last_write_time(path, error) != modified || error) { active = false; return false; }
    return true;
  }
  size_t read(uint64_t offset, char* output, size_t count) override {
    std::lock_guard<std::mutex> lock(mutex);
    if (!valid()) throw Error("MEDIA_CLOSED", "Media was released, removed or changed");
    if (offset >= length) return 0;
    count = static_cast<size_t>(std::min<uint64_t>(std::min<size_t>(count, 65536), length - offset));
#ifdef _WIN32
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD copied = 0;
    const bool success = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
      ReadFile(file, output, static_cast<DWORD>(count), &copied, nullptr) && copied == count;
#else
    file.clear(); file.seekg(static_cast<std::streamoff>(offset));
    file.read(output, static_cast<std::streamsize>(count));
    const bool success = file.gcount() == static_cast<std::streamsize>(count);
#endif
    if (!success) {
      active = false; throw Error("MEDIA_CLOSED", "Media read failed");
    }
    return count;
  }
};
Json WebResources::open_media(const fs::path& input) {
  std::error_code error;
  const auto path = fs::canonical(input, error);
  if (error || !fs::is_regular_file(path)) throw Error("FILE_NOT_FOUND", "Media path must be a regular file");
  auto extension = path.extension().u8string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const std::map<std::string, std::string> types{{".wav","audio/wav"},{".wave","audio/wav"},{".mp3","audio/mpeg"},
    {".flac","audio/flac"},{".ogg","audio/ogg"},{".oga","audio/ogg"},{".opus","audio/ogg"},{".m4a","audio/mp4"},
    {".mp4","video/mp4"},{".aac","audio/aac"},{".aif","audio/aiff"},{".aiff","audio/aiff"},{".webm","video/webm"}};
  const auto mime = types.find(extension);
  if (mime == types.end()) throw Error("UNSUPPORTED_FORMAT", "Unsupported media filename extension");
  std::lock_guard<std::mutex> lock(media_mutex_);
  if (media_.size() >= 32) throw Error("MEDIA_LIMIT", "At most 32 media files may be open per document");
  auto media = std::make_shared<Media>(path, mime->second);
  std::random_device random;
  const char* hex = "0123456789abcdef";
  std::string token;
  do {
    token.clear();
    for (int i = 0; i < 8; ++i) { auto n = random(); for (int j = 0; j < 8; ++j) { token += hex[n & 15]; n >>= 4; } }
  } while (media_.count(token));
  media_.emplace(token, media);
  return {{"url", origin_ + "/__reaweb_media__/" + token}, {"size", media->length}, {"type", media->type}};
}
bool WebResources::close_media(const std::string& url) {
  const auto prefix = origin_ + "/__reaweb_media__/";
  if (url.compare(0, prefix.size(), prefix) != 0) throw Error("INVALID_ARGUMENT", "Expected a media URL owned by this document");
  std::lock_guard<std::mutex> lock(media_mutex_);
  const auto it = media_.find(url.substr(prefix.size()));
  if (it == media_.end()) return false;
  it->second->active = false; media_.erase(it); return true;
}
void WebResources::clear_media() {
  std::lock_guard<std::mutex> lock(media_mutex_);
  for (auto& [token, media] : media_) media->active = false;
  media_.clear();
}
ResourceResponse WebResources::media_request(const std::string& token, const std::string& method,
    const std::map<std::string, std::string>& headers) const {
  ResourceResponse response;
  response.headers = {{"Cache-Control", "no-store"}, {"Content-Security-Policy", "default-src 'none'; sandbox"},
    {"X-Content-Type-Options", "nosniff"}, {"Cross-Origin-Resource-Policy", "same-origin"}, {"Content-Type", "application/octet-stream"}};
  auto fail = [&](int status) { response.status = status; response.headers["Content-Length"] = "0"; return response; };
  std::shared_ptr<Media> media;
  { std::lock_guard<std::mutex> lock(media_mutex_); auto it = media_.find(token); if (it != media_.end()) media = it->second; }
  if (!media || !media->valid()) return fail(404);
  uint64_t offset = 0, length = media->length;
  response.headers["Content-Type"] = media->type;
  response.headers["Accept-Ranges"] = "bytes";
  auto it = headers.find("range");
  if (method == "GET" && it != headers.end() && !headers.count("if-range")) {
    httplib::Ranges ranges;
    if (!httplib::detail::parse_range_header(it->second, ranges)) return fail(400);
    if (ranges.size() == 1) {
      auto [first, last] = ranges.front();
      if (first == -1) { first = std::max<int64_t>(0, static_cast<int64_t>(length) - last); last = static_cast<int64_t>(length) - 1; }
      else if (last == -1 || last >= static_cast<int64_t>(length)) last = static_cast<int64_t>(length) - 1;
      if (first < 0 || static_cast<uint64_t>(first) >= length || last < first) {
        response.headers["Content-Range"] = "bytes */" + std::to_string(length); return fail(416);
      }
      response.status = 206;
      response.headers["Content-Range"] = "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(length);
      offset = static_cast<uint64_t>(first); length = static_cast<uint64_t>(last - first + 1);
    }
  }
  response.headers["Content-Length"] = std::to_string(length);
  if (method == "GET") {
    if (media_reads_->fetch_add(1) >= 64) { --*media_reads_; return fail(429); }
    struct Lease final : ResourceReader {
      std::shared_ptr<Media> media;
      std::shared_ptr<std::atomic<size_t>> count;
      Lease(std::shared_ptr<Media> m, std::shared_ptr<std::atomic<size_t>> c) : media(std::move(m)), count(std::move(c)) {}
      ~Lease() override { --*count; }
      size_t read(uint64_t at, char* out, size_t size) override { return media->read(at, out, size); }
    };
    try { response.reader = std::make_shared<Lease>(media, media_reads_); }
    catch (...) { --*media_reads_; throw; }
    response.offset = offset; response.size = static_cast<size_t>(length);
  }
  return response;
}
std::string WebResources::entry_url(const fs::path& entry) const {
  const auto resolved = fs::canonical(entry);
  if (!contained(root_, resolved)) throw Error("INVALID_PATH", "The entry must be inside the App directory");
  return origin_ + "/" + encode_path(resolved.lexically_relative(root_).generic_u8string());
}
void WebResources::configure_loopback(const std::string& origin, const std::string& authorization) {
  if (!isolation_required_ || origin.rfind("http://127.0.0.1:", 0) != 0 || authorization.empty())
    throw Error("APP_ORIGIN_INVALID", "Invalid isolated App origin");
  origin_ = origin;
  authorization_ = authorization;
}
ResourceResponse WebResources::request(const std::string& uri, const std::string& method,
    const std::map<std::string, std::string>& request_headers) const {
  ResourceResponse response;
  response.headers = {{"Cache-Control", "no-cache"}, {"X-Content-Type-Options", "nosniff"},
    {"Cross-Origin-Resource-Policy", "same-origin"}, {"Content-Security-Policy", "frame-ancestors 'none'"},
    {"X-Frame-Options", "DENY"}};
  if (isolation_required_) {
    response.headers["Cross-Origin-Opener-Policy"] = "same-origin";
    response.headers["Cross-Origin-Embedder-Policy"] = "require-corp";
    response.headers["Permissions-Policy"] = "cross-origin-isolated=(self)";
  }
  auto fail = [&](int status) {
    response.status = status; response.headers["Content-Length"] = "0";
    response.headers["Content-Type"] = "text/plain; charset=utf-8";
    response.owner.reset(); response.data = nullptr; response.size = 0;
    return response;
  };
  auto header = [&](const char* name) {
    auto it = request_headers.find(name);
    return it == request_headers.end() ? std::string() : it->second;
  };
  if (uri.compare(0, origin_.size() + 1, origin_ + "/") != 0 ||
      (!header("host").empty() && header("host") != origin_.substr(9)) ||
      (!header("origin").empty() && header("origin") != origin_) ||
      (!header("sec-fetch-site").empty() && header("sec-fetch-site") != "same-origin" && header("sec-fetch-site") != "none"))
    return fail(403);
  if (method != "GET" && method != "HEAD") { response.headers["Allow"] = "GET, HEAD"; return fail(405); }
  try {
    const auto end = uri.find_first_of("?#", origin_.size());
    auto path = decode_path(uri.substr(origin_.size(), end == std::string::npos ? end : end - origin_.size()));
    if (path == "/__reaweb_runtime__/probe.js") {
      response.headers["Content-Type"] = "text/javascript; charset=utf-8";
      response.headers["Content-Length"] = std::to_string(sizeof(browser_probe_script) - 1);
      if (method == "GET") { response.data = browser_probe_script; response.size = sizeof(browser_probe_script) - 1; }
      return response;
    }
    const std::string media_prefix = "/__reaweb_media__/";
    if (path.compare(0, media_prefix.size(), media_prefix) == 0)
      return media_request(path.substr(media_prefix.size()), method, request_headers);
    if (path.back() == '/') path += "index.html";
    auto relative = fs::u8path(path.substr(1));
    if (relative.has_root_path()) return fail(403);
    for (const auto& component : relative)
      if (component == ".." || same_binding_path(component, ".reawebapi")) return fail(403);
    std::error_code error;
    const auto resolved = fs::weakly_canonical(root_ / relative, error);
    if (error || !contained(root_, resolved)) return fail(403);
    const auto private_root = root_ / ".reawebapi";
    if (same_binding_path(resolved, private_root) || binding_contains(private_root, resolved)) return fail(403);
    if (fs::equivalent(resolved.parent_path(), private_root, error) ||
        fs::equivalent(resolved, private_root / "identity.json", error)) return fail(403);
    std::string etag;
    time_t modified = -1;
    if (!fs::exists(resolved) && path == "/.well-known/appspecific/com.chrome.devtools.json") {
      auto body = std::make_shared<const std::string>("{}");
      response.owner = body; response.data = body->data(); response.size = body->size();
    } else {
      if (!fs::is_regular_file(resolved)) return fail(404);
      auto file = std::make_shared<httplib::detail::mmap>(resolved.u8string().c_str());
      if (!file->is_open()) return fail(404);
      response.owner = file; response.data = file->data(); response.size = file->size();
      etag = "\"" + std::to_string(response.size) + "-" + file_time_ticks(fs::last_write_time(resolved).time_since_epoch().count()) + "\"";
      response.headers["ETag"] = etag;
      modified = httplib::detail::FileStat(resolved.u8string()).mtime();
      response.headers["Last-Modified"] = httplib::detail::file_mtime_to_http_date(modified);
    }
    response.headers["Content-Type"] = httplib::detail::find_content_type(path,
      {{"js", "text/javascript; charset=utf-8"}, {"mjs", "text/javascript; charset=utf-8"},
       {"json", "application/json; charset=utf-8"}, {"wasm", "application/wasm"}}, "application/octet-stream");
    response.headers["Accept-Ranges"] = "bytes";
    const auto match = header("if-none-match");
    const auto since = httplib::detail::parse_http_date(header("if-modified-since"));
    const bool not_modified = !match.empty() ? httplib::detail::split_find(match.data(), match.data() + match.size(), ',',
      [&](const char* first, const char* last) {
        std::string tag(first, last);
        if (tag.compare(0, 2, "W/") == 0) tag.erase(0, 2);
        return !etag.empty() && (tag == etag || tag == "*");
      }) : modified != -1 && since != -1 && modified <= since;
    if (not_modified) {
      response.status = 304; response.owner.reset(); response.data = nullptr; response.size = 0;
      return response;
    }
    const auto range = header("range");
    const auto if_range = header("if-range");
    const auto range_date = httplib::detail::parse_http_date(if_range);
    if (method == "GET" && !range.empty() && (if_range.empty() || if_range == etag ||
        (modified != -1 && range_date != -1 && modified <= range_date))) {
      httplib::Ranges ranges;
      if (!httplib::detail::parse_range_header(range, ranges)) return fail(400);
      // Multiple ranges may legally be ignored. Single ranges cover media seeking.
      if (ranges.size() == 1) {
        const auto length = static_cast<int64_t>(response.size);
        auto [first, last] = ranges.front();
        if (first == -1) { first = std::max<int64_t>(0, length - last); last = length - 1; }
        else if (last == -1 || last >= length) last = length - 1;
        if (first < 0 || first >= length || last < first) {
          response.headers["Content-Range"] = "bytes */" + std::to_string(length); return fail(416);
        }
        response.status = 206;
        response.headers["Content-Range"] = "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(length);
        response.data += first; response.size = static_cast<size_t>(last - first + 1);
      }
    }
    response.headers["Content-Length"] = std::to_string(response.size);
    if (method == "HEAD") { response.owner.reset(); response.data = nullptr; response.size = 0; }
    return response;
  } catch (const Error&) { return fail(400); }
  catch (const std::exception&) { return fail(500); }
}
}
