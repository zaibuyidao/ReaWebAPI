#include "runtime/diagnostics.hpp"
#include "runtime/external_settings.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace reaweb {
namespace {
std::string trim(std::string text) {
  const auto first = text.find_first_not_of(" \t\r"), last = text.find_last_not_of(" \t\r");
  return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}
std::string read_ini(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return {};
  if (std::filesystem::file_size(path) > 1024 * 1024) throw Error("SETTINGS_INVALID", "ReaWebAPI.ini exceeds 1 MiB");
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw Error("SETTINGS_IO", "Cannot read ReaWebAPI.ini");
  return {std::istreambuf_iterator<char>(stream), {}};
}
bool valid_target(const std::string& value) {
  return value.size() <= 240 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;
}
}
DiagnosticSettings read_diagnostic_settings(const std::filesystem::path& file) noexcept {
  try {
    DiagnosticSettings settings;
    std::istringstream input(read_ini(file)); std::string line; bool section = false;
    while (std::getline(input, line)) {
      line = trim(line);
      if (!line.empty() && line[0] == '[') { section = line == "[Diagnostics]"; continue; }
      const auto equals = line.find('=');
      if (!section || equals == std::string::npos) continue;
      const auto key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
      if (key == "AppDebug") settings.app_debug = value == "1";
      if (key == "RuntimeDebug") settings.runtime_debug = value == "1";
      if (key == "TargetAppId") settings.target = value;
    }
    if (!valid_target(settings.target)) return {};
    return settings;
  } catch (...) { return {}; }
}
void write_diagnostic_settings(const std::filesystem::path& file, const DiagnosticSettings& settings) {
  if (!valid_target(settings.target)) throw Error("SETTINGS_INVALID", "Target App ID must use lowercase a-z, 0-9 and -, up to 240 characters.");
  if (settings.app_debug && settings.target.empty()) throw Error("SETTINGS_INVALID", "Enter a Target App ID to enable App debug logging.");
  std::istringstream input(read_ini(file)); std::string line, output; bool section = false;
  while (std::getline(input, line)) {
    const auto text = trim(line);
    if (!text.empty() && text[0] == '[') section = text == "[Diagnostics]";
    if (!section) output += line + "\n";
  }
  output += "[Diagnostics]\nAppDebug=" + std::string(settings.app_debug ? "1" : "0") +
    "\nRuntimeDebug=" + (settings.runtime_debug ? "1" : "0") + "\nTargetAppId=" + settings.target + "\n";
  auto temporary = file; temporary += ".diagnostics.tmp";
  try {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
#ifndef _WIN32
    std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
    stream << output; stream.close();
    if (!stream) throw Error("SETTINGS_IO", "Cannot save diagnostics settings.");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw Error("SETTINGS_IO", "Cannot replace ReaWebAPI.ini");
#else
    std::filesystem::rename(temporary, file);
#endif
  } catch (...) { std::error_code ignored; std::filesystem::remove(temporary, ignored); throw; }
}
Diagnostics::Diagnostics(const std::filesystem::path& resource) noexcept {
  try {
    directory_ = resource / "ReaWebAPI" / "Diagnostics";
    session_ = external_token().substr(0, 24);
    settings_ = read_diagnostic_settings(resource / "ReaWebAPI.ini");
  } catch (...) { directory_.clear(); }
}
void Diagnostics::configure(const DiagnosticSettings& settings) noexcept {
  if (std::this_thread::get_id() != owner_) return;
  try { settings_ = settings; rates_.clear(); } catch (...) {}
}
bool Diagnostics::enabled(Level level, Scope scope, const std::string& app) const noexcept {
  if (std::this_thread::get_id() != owner_ || directory_.empty()) return false;
  return level == Level::Error || level == Level::Warn ||
    (settings_.app_debug && !app.empty() && app == settings_.target) ||
    (scope == Scope::Runtime && settings_.runtime_debug);
}
std::string Diagnostics::filename(const std::string& app) {
  // Existing App IDs are lowercase ASCII. A prefix also avoids Windows device names.
  if (app.empty()) return "runtime.log";
  if (!valid_target(app)) throw Error("INVALID_ARGUMENT", "Invalid diagnostic App ID");
  return "app-" + app + ".log";
}
void Diagnostics::write(const std::filesystem::path& path, const std::string& line) {
  namespace fs = std::filesystem;
  fs::create_directories(directory_);
  if (fs::is_symlink(fs::symlink_status(path))) return;
  if (fs::exists(path) && fs::file_size(path) + line.size() > file_limit) {
    for (int n = 3; n >= 1; --n) {
      auto source = path; if (n > 1) source += "." + std::to_string(n - 1);
      auto destination = path; destination += "." + std::to_string(n);
      if (fs::exists(destination)) fs::remove(destination);
      if (fs::exists(source)) fs::rename(source, destination);
    }
  }
  if (!fs::exists(path)) {
    std::vector<fs::directory_entry> files;
    for (const auto& entry : fs::directory_iterator(directory_)) {
      const auto name = entry.path().filename().string();
      const auto suffix = name.find(".log");
      if (!entry.is_symlink() && entry.is_regular_file() &&
          (name.rfind("app-", 0) == 0 || name.rfind("runtime.log", 0) == 0) && suffix != std::string::npos &&
          (name.substr(suffix) == ".log" || name.substr(suffix) == ".log.1" || name.substr(suffix) == ".log.2" || name.substr(suffix) == ".log.3")) files.push_back(entry);
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.last_write_time() < b.last_write_time(); });
    for (size_t i = 0; files.size() - i >= file_count; ++i) fs::remove(files[i]);
  }
  std::ofstream stream(path, std::ios::binary | std::ios::app);
#ifndef _WIN32
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
#endif
  stream << line; stream.flush();
}
void Diagnostics::event(Level level, Scope scope, const char* event_name, const std::string& app, int window,
    const char* result, const Json& fields) noexcept {
  if (!enabled(level, scope, app)) return;
  try {
    const auto key = app + ":" + std::to_string(window) + ":" + event_name;
    if (rates_.size() >= 1024 && !rates_.count(key)) rates_.clear();
    auto& rate = rates_[key]; const auto now = Clock::now();
    if (now - rate.start >= std::chrono::seconds(1)) { rate.start = now; rate.count = 0; }
    if (++rate.count > 20) { ++rate.suppressed; return; }
    const auto wall = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(wall);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream timestamp, thread;
    timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') <<
      std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count() % 1000 << 'Z';
    thread << owner_;
    const char* levels[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    Json entry{{"time", timestamp.str()}, {"level", levels[static_cast<int>(level)]},
      {"scope", scope == Scope::Runtime ? "runtime" : "app"}, {"event", event_name}, {"sessionId", session_},
      {"appId", app}, {"windowId", window}, {"result", result}, {"threadId", thread.str()}};
    // Only structured, explicitly selected metadata is accepted. Never persist free-form messages or payloads.
    for (const auto* field : {"generation", "fxGuid", "fxIndex", "fxLocation", "fxSessionId", "activation", "durationMs", "count", "revision", "method",
        "pendingCalls", "queuedCalls", "outputPending", "hostMessages", "webMessages", "pagePending", "oldestAgeMs", "oldestStarted", "streamPackets", "activeStreams",
        "timeouts", "queueRejected", "sendFailures", "scriptErrors", "promiseErrors", "callbackErrors", "streamErrors", "streamCloses", "service", "operation",
        "scriptLine", "scriptColumn", "timeoutMethod"})
      if (fields.is_object() && fields.contains(field)) entry[field] = fields[field];
    if (rate.suppressed) { entry["suppressed"] = rate.suppressed; rate.suppressed = 0; }
    for (const auto* field : {"method", "service", "operation", "timeoutMethod"}) if (entry.contains(field)) {
      const auto value = entry[field].get<std::string>();
      if (value.empty() || value.size() > 128 || value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos) entry.erase(field);
    }
    if (std::string(result).size() > 64 || std::string(result).find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
      entry["result"] = "error";
    const auto line = entry.dump(-1, ' ', true, Json::error_handler_t::replace) + '\n';
    if (line.size() > 8192) return;
    if (scope == Scope::Runtime) write(directory_ / "runtime.log", line);
    if (!app.empty()) write(directory_ / filename(app), line);
    else if (scope == Scope::App) write(directory_ / "runtime.log", line);
  } catch (...) {}
}
}
