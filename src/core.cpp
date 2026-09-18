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
double track_value(const std::string& key, const Json& input) {
  if (!input.is_number()) throw Error("INVALID_ARGUMENT", "value must be a finite number");
  const auto value = input.get<double>();
  if (!std::isfinite(value) || (key == "D_VOL" && value < 0) ||
      (key == "D_PAN" && (value < -1 || value > 1)) ||
      (key == "B_MUTE" && value != 0 && value != 1) ||
      (key == "I_SOLO" && (value < 0 || value > 2 || value != std::floor(value))))
    throw Error("INVALID_ARGUMENT", "Track value is outside the supported range");
  return value;
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
    auto ok = host_.set_track_value(track(a[0]), key, track_value(key, a[2]));
    if (ok && !batching_ && host_.update_arrange) host_.update_arrange();
    return ok;
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
    return Json{{"version", REAWEB_VERSION}, {"protocol", 1}, {"methods", names}, {"projectScope", "current"},
      {"events", {"projectchange", "selectionchange", "windowstatechange"}},
      {"limits", {{"requestBytes", 65536}, {"batchCalls", 32}, {"pendingCalls", 256}}}};
  });
  add("ReaWeb_Batch", 1, 2, [this](const Json& a) { return batch(a); });
  for (const auto& name : {"ReaWeb_GetWindowState", "ReaWeb_GetDiagnostics", "ReaWeb_Focus"})
    add(name, 0, 0, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto& name : {"ReaWeb_SetTitle", "ReaWeb_SetKeyboardCapture", "ReaWeb_Subscribe", "ReaWeb_Unsubscribe"})
    add(name, 1, 1, [this, name](const Json& a) { return controls_.host_call(name, a); });
}

void Bridge::add(const std::string& name, size_t min, size_t max, std::function<Json(const Json&)> fn) {
  methods_.emplace(name, Method{min, max, std::move(fn)});
}

void Bridge::observe_project() {
  auto current = host_.current_project();
  if (project_ != current) { reset_handles(); project_ = current; }
}
void Bridge::reset_handles() { handles_.clear(); track_tokens_.clear(); handle_scan_.clear(); project_ = nullptr; }

Json Bridge::batch(const Json& args) {
  const auto& calls = args[0];
  if (!calls.is_array() || calls.empty() || calls.size() > 32)
    throw Error("INVALID_ARGUMENT", "A batch must contain 1 to 32 calls");
  std::string label;
  if (args.size() > 1) {
    if (!args[1].is_object()) throw Error("INVALID_ARGUMENT", "Expected batch options");
    for (const auto& item : args[1].items())
      if (item.key() != "undoLabel") throw Error("INVALID_ARGUMENT", "Unknown batch option: " + item.key());
    if (args[1].contains("undoLabel")) {
      label = string_arg(args[1]["undoLabel"]);
      if (label.empty() || label.size() > 256) throw Error("INVALID_ARGUMENT", "undoLabel must contain 1 to 256 UTF-8 bytes");
    }
  }
  bool writes = false;
  // Validate every call before starting Undo or changing the project. No nested or window calls.
  for (const auto& call : calls) {
    if (!call.is_object() || !call.contains("method") || !call["method"].is_string() ||
        !call.contains("args") || !call["args"].is_array()) throw Error("INVALID_ARGUMENT", "Invalid batch call");
    const auto name = call["method"].get<std::string>();
    const auto& a = call["args"];
    const auto method = methods_.find(name);
    if (method == methods_.end() || name.rfind("ReaWeb", 0) == 0)
      throw Error("INVALID_ARGUMENT", "This API cannot be batched: " + name);
    if (a.size() < method->second.min_args || a.size() > method->second.max_args)
      throw Error("INVALID_ARGUMENT", "Wrong batch argument count");
    if (name == "GetTrackName" || name == "GetMediaTrackInfo_Value" || name == "SetMediaTrackInfo_Value") {
      track(a[0]);
      if (a.size() >= 2) track_key(a[1]);
      if (name == "SetMediaTrackInfo_Value") { track_value(track_key(a[1]), a[2]); writes = true; }
    } else if (name != "GetAppVersion") {
      project(a);
      if (a.size() > 1) integer(a[1], "index");
    }
  }
  if (writes && !label.empty() && (!host_.begin_undo || !host_.end_undo))
    throw Error("UNDO_UNAVAILABLE", "The host does not support Undo groups");
  Json results = Json::array();
  bool undo = false, refresh = false;
  // All operations complete synchronously on the main thread, including failure cleanup.
  auto finish = [&] {
    batching_ = false;
    std::exception_ptr error;
    if (refresh) {
      refresh = false;
      try { host_.prevent_refresh(-1); } catch (...) { error = std::current_exception(); }
    }
    if (undo) {
      undo = false;
      try { host_.end_undo(project_, label); } catch (...) { if (!error) error = std::current_exception(); }
    }
    if (writes && host_.update_arrange) {
      try { host_.update_arrange(); } catch (...) { if (!error) error = std::current_exception(); }
    }
    if (error) std::rethrow_exception(error);
  };
  try {
    if (writes && !label.empty()) { host_.begin_undo(project_); undo = true; }
    if (writes && host_.prevent_refresh) { host_.prevent_refresh(1); refresh = true; }
    batching_ = true;
    for (const auto& call : calls) {
      auto result = methods_.at(call["method"].get<std::string>()).invoke(call["args"]);
      if (call["method"] == "SetMediaTrackInfo_Value" && result == false)
        throw Error("NATIVE_ERROR", "REAPER rejected the track value");
      results.push_back(std::move(result));
    }
  } catch (const std::exception& e) {
    const auto message = std::string(e.what());
    Json details{{"completed", results.size()}, {"results", results}, {"rolledBack", false}};
    try { finish(); } catch (const std::exception& cleanup) { details["cleanupError"] = cleanup.what(); }
    throw Error("BATCH_FAILED", message, std::move(details));
  }
  try { finish(); }
  catch (const std::exception& e) {
    throw Error("BATCH_FAILED", e.what(), {{"completed", results.size()}, {"results", results}, {"rolledBack", false}});
  }
  return results;
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
  if (auto token = track_tokens_.find(pointer); token != track_tokens_.end()) {
    auto existing = handles_.find(token->second);
    if (existing != handles_.end() && existing->second.guid == guid)
      return Json{{"type", "MediaTrack"}, {"id", token->second}};
    if (existing != handles_.end()) handles_.erase(existing);
    track_tokens_.erase(token);
  }
  // Bound cleanup work. Enumerating a large project must not rescan every existing handle.
  for (int n = 0; n < 8 && !handle_scan_.empty(); ++n) {
    auto id = std::move(handle_scan_.front()); handle_scan_.pop_front();
    auto it = handles_.find(id);
    if (it == handles_.end()) continue;
    if (!host_.valid_track(project_, it->second.pointer) || host_.track_guid(it->second.pointer) != it->second.guid) {
      track_tokens_.erase(it->second.pointer); handles_.erase(it);
    } else handle_scan_.push_back(std::move(id));
  }
  if (handles_.size() >= 65536) throw Error("HANDLE_LIMIT", "Too many live object handles");
  auto id = session_ + ":" + std::to_string(++next_handle_);
  handles_.emplace(id, Handle{pointer, project_, guid});
  track_tokens_[pointer] = id;
  handle_scan_.push_back(id);
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
    track_tokens_.erase(h.pointer);
    handles_.erase(it);
    throw Error("STALE_HANDLE", "Track was deleted or its project changed; get the track again");
  }
  return h.pointer;
}

Json parse_request(const std::string& message) {
    if (message.size() > 65536) throw Error("MESSAGE_LIMIT", "Bridge message exceeds 64 KiB");
    const auto request = Json::parse(message, [](int depth, Json::parse_event_t, Json&) {
      if (depth > 64) throw Error("INVALID_REQUEST", "JSON nesting exceeds 64 levels");
      return true;
    });
    if (!request.is_object() || !request.contains("id") || !request["id"].is_number_integer() ||
        request["id"].get<double>() < 1 || request["id"].get<double>() > 9007199254740991.0)
      throw Error("INVALID_REQUEST", "Expected a positive safe integer request id");
    if (request.contains("document") && (!request["document"].is_string() ||
        request["document"].get_ref<const std::string&>().empty() || request["document"].get_ref<const std::string&>().size() > 128))
      throw Error("INVALID_REQUEST", "Invalid document token");
    if (!request.contains("method") || !request["method"].is_string() ||
        !request.contains("args") || !request["args"].is_array())
      throw Error("INVALID_REQUEST", "Expected method and args");
    return request;
}
Json error_response(const Json& request, const std::string& code, const std::string& message, Json details) {
  Json error{{"code", code}, {"message", message}};
  if (!details.is_null()) error["details"] = std::move(details);
  return Json{{"id", request.is_object() ? request.value("id", Json()) : Json()},
    {"document", request.is_object() ? request.value("document", Json()) : Json()}, {"error", std::move(error)}};
}
Json Bridge::dispatch(const std::string& message) {
  try { return dispatch_request(parse_request(message)); }
  catch (const Error& e) { return error_response(Json(), e.code, e.what(), e.details); }
  catch (const Json::exception&) { return error_response(Json(), "INVALID_REQUEST", "Invalid JSON request or argument type"); }
}
Json Bridge::dispatch_request(const Json& request) {
  try {
    auto method = methods_.find(request.at("method").get<std::string>());
    if (method == methods_.end()) throw Error("UNKNOWN_API", "API is not registered in this runtime");
    const auto& args = request["args"];
    if (args.size() < method->second.min_args || args.size() > method->second.max_args)
      throw Error("INVALID_ARGUMENT", "Wrong argument count");
    observe_project();
    return Json{{"id", request["id"]}, {"document", request.value("document", Json())}, {"result", method->second.invoke(args)}};
  } catch (const Error& e) {
    return error_response(request, e.code, e.what(), e.details);
  } catch (const Json::exception&) {
    return error_response(request, "INVALID_REQUEST", "Invalid JSON request or argument type");
  } catch (const std::exception& e) {
    return error_response(request, "NATIVE_ERROR", e.what());
  }
}
}
