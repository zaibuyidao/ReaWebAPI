#include "core.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace reaweb {
namespace {
int integer(const Json& value, const char* label) {
  if (!value.is_number_integer() || value.get<double>() < 0 ||
      value.get<double>() > std::numeric_limits<int>::max())
    throw Error("INVALID_ARGUMENT", std::string(label) + " must be a non-negative integer");
  return value.get<int>();
}
std::string string_arg(const Json& value) {
  if (!value.is_string()) throw Error("INVALID_ARGUMENT", "Expected a string");
  auto str = value.get<std::string>();
  if (str.find('\0') != std::string::npos) throw Error("INVALID_ARGUMENT", "Embedded NUL is not allowed");
  return str;
}
std::string track_key(const Json& value) {
  auto key = string_arg(value);
  if (key != "D_VOL" && key != "D_PAN" && key != "B_MUTE" && key != "I_SOLO")
    throw Error("UNSUPPORTED_PARAMETER", "Supported track values: D_VOL, D_PAN, B_MUTE, I_SOLO");
  return key;
}
}

fs::path resolve_html(const fs::path& base, const std::string& input) {
  if (input.empty() || input.size() > 32768 || input.find('\0') != std::string::npos ||
      input.find("://") != std::string::npos)
    throw Error("INVALID_PATH", "Expected a local HTML file path");
  auto path = fs::u8path(input);
  if (!path.is_absolute()) path = base / path;
  std::error_code ec;
  path = fs::canonical(path, ec);
  if (ec || !fs::is_regular_file(path)) throw Error("INVALID_PATH", "HTML file does not exist: " + input);
  auto ext = path.extension().u8string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (ext != ".html" && ext != ".htm") throw Error("INVALID_PATH", "Entry point must be .html or .htm");
  return path;
}

std::string file_uri(const fs::path& path) {
  const auto raw = path.generic_u8string();
  std::string uri = raw.front() == '/' ? "file://" : "file:///";
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : raw) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '/' || c == ':' || c == '-' || c == '_' || c == '.' || c == '~') uri += static_cast<char>(c);
    else { uri += '%'; uri += hex[c >> 4]; uri += hex[c & 15]; }
  }
  return uri;
}

bool same_document(const std::string& uri, const std::string& entry) {
  return uri.substr(0, uri.find('#')) == entry.substr(0, entry.find('#'));
}

Bridge::Bridge(Host& host, Controls controls, std::string session)
  : host_(host), controls_(std::move(controls)), session_(std::move(session)) {
  add("CountTracks", 0, 1, [this](const Json& a) { return host_.count_tracks(project(a)); });
  add("CountSelectedTracks", 0, 1, [this](const Json& a) { return host_.count_selected_tracks(project(a)); });
  add("GetTrack", 2, 2, [this](const Json& a) {
    return track_handle(host_.get_track(project(a), integer(a[1], "index")));
  });
  add("GetSelectedTrack", 0, 2, [this](const Json& a) {
    return track_handle(host_.get_selected_track(project(a), a.size() > 1 ? integer(a[1], "index") : 0));
  });
  add("GetTrackName", 1, 1, [this](const Json& a) { return host_.track_name(track(a[0])); });
  add("GetMediaTrackInfo_Value", 2, 2, [this](const Json& a) {
    auto key = track_key(a[1]);
    return host_.get_track_value(track(a[0]), key);
  });
  add("SetMediaTrackInfo_Value", 3, 3, [this](const Json& a) {
    auto key = track_key(a[1]);
    if (!a[2].is_number()) throw Error("INVALID_ARGUMENT", "value must be a finite number");
    const auto value = a[2].get<double>();
    if (!std::isfinite(value) || (key == "D_VOL" && value < 0) ||
        (key == "D_PAN" && (value < -1 || value > 1)) ||
        (key == "B_MUTE" && value != 0 && value != 1) ||
        (key == "I_SOLO" && (value < 0 || value > 2 || value != std::floor(value))))
      throw Error("INVALID_ARGUMENT", "Track value is outside the supported range");
    return host_.set_track_value(track(a[0]), key, value);
  });
  add("GetAppVersion", 0, 0, [this](const Json&) { return host_.version(); });
  add("ReaWebOpen", 1, 1, [this](const Json& a) { return controls_.open(string_arg(a[0])); });
  add("ReaWeb_Close", 0, 0, [this](const Json&) { controls_.close(); return true; });
  add("ReaWeb_DevTools", 0, 0, [this](const Json&) { controls_.devtools(); return true; });
  add("ReaWeb_SetDocked", 1, 1, [this](const Json& a) {
    if (!a[0].is_boolean()) throw Error("INVALID_ARGUMENT", "docked must be a boolean");
    return controls_.set_docked(a[0].get<bool>());
  });
  add("ReaWeb_IsDocked", 0, 0, [this](const Json&) { return controls_.is_docked(); });
  add("ReaWeb_GetCapabilities", 0, 0, [this](const Json&) {
    Json names = Json::array();
    for (const auto& entry : methods_) names.push_back(entry.first);
    return Json{{"version", REAWEB_VERSION}, {"methods", names}, {"projectScope", "current"}};
  });
}

void Bridge::add(const std::string& name, size_t min, size_t max, std::function<Json(const Json&)> fn) {
  methods_.emplace(name, Method{min, max, std::move(fn)});
}

void Bridge::observe_project() {
  auto current = host_.current_project();
  if (project_ != current) { handles_.clear(); project_ = current; }
}

void* Bridge::project(const Json& args) {
  if (!args.empty() && !args[0].is_null() && !(args[0].is_number_integer() && args[0] == 0))
    throw Error("UNSUPPORTED_PROJECT", "This release supports only the current project (0 or null)");
  return project_;
}

Json Bridge::track_handle(void* pointer) {
  if (!pointer) return nullptr;
  if (!host_.valid_track(project_, pointer)) throw Error("STALE_HANDLE", "Track no longer exists");
  auto guid = host_.track_guid(pointer);
  // Reuse a live token so long-running polling does not grow the table.
  for (auto it = handles_.begin(); it != handles_.end();) {
    if (!host_.valid_track(project_, it->second.pointer)) it = handles_.erase(it);
    else {
      if (it->second.pointer == pointer && it->second.guid == guid)
        return Json{{"type", "MediaTrack"}, {"id", it->first}};
      ++it;
    }
  }
  if (handles_.size() >= 65536) throw Error("HANDLE_LIMIT", "Too many live object handles");
  auto id = session_ + ":" + std::to_string(++next_handle_);
  handles_.emplace(id, Handle{pointer, project_, guid});
  return Json{{"type", "MediaTrack"}, {"id", id}};
}

void* Bridge::track(const Json& value) {
  if (!value.is_object() || value.value("type", "") != "MediaTrack" ||
      !value.contains("id") || !value["id"].is_string())
    throw Error("INVALID_HANDLE", "Expected a MediaTrack handle");
  auto it = handles_.find(value["id"].get<std::string>());
  if (it == handles_.end()) throw Error("STALE_HANDLE", "Unknown handle; get the track again");
  const auto& h = it->second;
  if (h.project != project_ || !host_.valid_track(project_, h.pointer) || host_.track_guid(h.pointer) != h.guid) {
    handles_.erase(it);
    throw Error("STALE_HANDLE", "Track was deleted or its project changed; get the track again");
  }
  return h.pointer;
}

Json Bridge::dispatch(const std::string& message) {
  Json id = nullptr;
  Json document = nullptr;
  try {
    if (message.size() > 65536) throw Error("MESSAGE_LIMIT", "Bridge message exceeds 64 KiB");
    const auto request = Json::parse(message, [](int depth, Json::parse_event_t, Json&) {
      if (depth > 64) throw Error("INVALID_REQUEST", "JSON nesting exceeds 64 levels");
      return true;
    });
    if (!request.is_object() || !request.contains("id") || !request["id"].is_number_integer() ||
        request["id"].get<double>() < 1 || request["id"].get<double>() > 9007199254740991.0)
      throw Error("INVALID_REQUEST", "Expected a positive safe integer request id");
    id = request["id"];
    if (request.contains("document") && request["document"].is_string() && request["document"].get_ref<const std::string&>().size() <= 128)
      document = request["document"];
    if (!request.contains("method") || !request["method"].is_string() ||
        !request.contains("args") || !request["args"].is_array())
      throw Error("INVALID_REQUEST", "Expected method and args");
    auto method = methods_.find(request["method"].get<std::string>());
    if (method == methods_.end()) throw Error("UNKNOWN_API", "API is not registered in this runtime");
    const auto& args = request["args"];
    if (args.size() < method->second.min_args || args.size() > method->second.max_args)
      throw Error("INVALID_ARGUMENT", "Wrong argument count");
    observe_project();
    return Json{{"id", id}, {"document", document}, {"result", method->second.invoke(args)}};
  } catch (const Error& e) {
    return Json{{"id", id}, {"document", document}, {"error", {{"code", e.code}, {"message", e.what()}}}};
  } catch (const Json::exception&) {
    return Json{{"id", id}, {"document", document}, {"error", {{"code", "INVALID_REQUEST"}, {"message", "Invalid JSON request or argument type"}}}};
  } catch (const std::exception& e) {
    return Json{{"id", id}, {"document", document}, {"error", {{"code", "NATIVE_ERROR"}, {"message", e.what()}}}};
  }
}
}
