#include "core/core.hpp"
#include "core/native.hpp"
#include "runtime/services.hpp"
#include "core/batch.hpp"
#include <regex>
#include "api_schema.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace reaweb {
namespace {
const Json& api_schema() {
  // Validated, immutable schema projection embedded by the offline build step.
  static const Json schema = Json::parse(api_schema_json);
  return schema;
}

std::string string_arg(const Json& value) {
  if (!value.is_string()) throw Error("INVALID_ARGUMENT", "Expected a string");
  auto str = value.get<std::string>();
  if (str.find('\0') != std::string::npos) throw Error("INVALID_ARGUMENT", "Embedded NUL is not allowed");
  return str;
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

std::string validate_dev_url(const std::string& url) {
  static const std::regex pattern(R"(^http://(127\.0\.0\.1|localhost|\[::1\]):([0-9]{1,5})(/[^\s\\]*)?$)");
  std::smatch match;
  if (url.size() > 8192 || std::any_of(url.begin(), url.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
      !std::regex_match(url, match, pattern))
    throw Error("INVALID_URL", "Development URLs must use http://127.0.0.1:port/ (or localhost/[::1])");
  const auto port = std::stoi(match[2]);
  if (port < 1 || port > 65535) throw Error("INVALID_URL", "Invalid development server port");
  return match[3].matched ? url : url + "/";
}
void validate_external_url(const std::string& url) {
  if (url.empty() || url.size() > 8192 || url.find('\\') != std::string::npos ||
      std::any_of(url.begin(), url.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
      !(url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0 || url.rfind("mailto:", 0) == 0))
    throw Error("INVALID_URL", "Only http, https and mailto external links are supported");
  const auto begin = url.rfind("mailto:", 0) == 0 ? 7u : url.find("://") + 3;
  if (begin == url.size() || url[begin] == '/' || url[begin] == '?' || url[begin] == '#')
    throw Error("INVALID_URL", "External links must include a destination");
}

Bridge::Bridge(Host& host, Controls controls, std::string session)
  : host_(host), controls_(std::move(controls)), session_(std::move(session)) {
  native_ = std::make_unique<NativeContext>(host_, session_);
  for (const auto& entry : native_entries()) {
    const auto* binding = &entry;
    add_reaper(entry.name, [this, binding](const Json& args) { return native_->invoke(*binding, args); });
  }
  if (methods_.size() != api_schema().at("bindings").size())
    throw Error("SCHEMA_MISMATCH", "Native API registry does not match the embedded schema");
  add("ReaWeb_Open", 1, 1, [this](const Json& a) { return controls_.open(string_arg(a[0])); });
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
    auto api = api_schema();
    api.erase("knownMethods");
    api.update(native_->capabilities());
    return Json{{"version", REAWEB_VERSION}, {"protocol", 1}, {"methods", names}, {"api", std::move(api)}, {"projectScope", "all"},
      {"events", runtime_events()},
      {"runtime", {{"contract", 2}, {"namespaces", {"window", "theme", "dialog", "events", "lifecycle", "debug", "fs", "audio",
                                                  "clipboard", "dragDrop", "app", "system", "transaction", "host"}},
        {"host", {{"maxMessageBytes", host_message_limit}, {"maxPendingMessages", host_queue_limit}, {"maxQueuedBytes", host_queue_bytes}}},
        {"reservedNamespaces", Json::array()},
        {"dragDrop", {{"maxFiles", 256}, {"maxTextBytes", value_limit}, {"effect", "copy"}}},
        {"cleanupTimeoutMs", 2000}, {"audio", {{"maxChannels", 32}, {"maxWaveformPoints", 8192}, {"maxPendingJobs", 8}}}}},
      {"batchMethods", batch_methods()},
      {"limits", {{"requestBytes", message_limit}, {"batchCalls", batch_limit}, {"pendingCalls", 256}}}};
  });
  add("ReaWeb_Batch", 1, 2, [this](const Json& a) { return batch(a); });
  add("ReaWeb_SetBufferSize", 1, 1, [this](const Json& a) { return native_->set_buffer_size([&]() -> size_t {
    if (!a[0].is_number_unsigned() && (!a[0].is_number_integer() || a[0].get<int64_t>() < 0))
      throw Error("INVALID_ARGUMENT", "bytes must be a positive integer");
    return a[0].get<size_t>();
  }()); });
  for (const auto& name : {"ReaWeb_GetWindowState", "ReaWeb_GetDiagnostics", "ReaWeb_Focus"})
    add(name, 0, 0, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto& name : {"ReaWeb_SetTitle", "ReaWeb_SetIcon", "ReaWeb_SetIconVisible", "ReaWeb_SetKeyboardCapture", "ReaWeb_Subscribe", "ReaWeb_Unsubscribe"})
    add(name, 1, 1, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto& name : {"ReaWeb_HostSend", "ReaWeb_OpenDev", "ReaWeb_BeginUndo", "ReaWeb_EndUndo", "ReaWeb_ClipboardWriteText", "ReaWeb_OpenExternal"})
    add(name, 1, 1, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto& name : {"ReaWeb_ClipboardReadText"})
    add(name, 0, 0, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto& name : {"ReaWeb_ReadFile", "ReaWeb_ReadDirectory", "ReaWeb_Stat", "ReaWeb_MakeDirectory"})
    add(name, 1, 2, [this, name](const Json& a) { return controls_.host_call(name, a); });
  add("ReaWeb_WriteFile", 2, 3, [this](const Json& a) { return controls_.host_call("ReaWeb_WriteFile", a); });
  for (const auto& name : {"ReaWeb_GetBounds", "ReaWeb_GetTheme", "ReaWeb_GetLogs", "ReaWeb_Reload", "ReaWeb_GetAppInfo", "ReaWeb_GetPlatform", "ReaWeb_GetArchitecture"})
    add(name, 0, 0, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto& name : {"ReaWeb_SetBounds", "ReaWeb_SetVisible", "ReaWeb_Log", "ReaWeb_LifecycleSubscribe", "ReaWeb_LifecycleComplete", "ReaWeb_AudioFileInfo", "ReaWeb_DragFiles", "ReaWeb_DragText", "ReaWeb_RevealPath"})
    add(name, 1, 1, [this, name](const Json& a) { return controls_.host_call(name, a); });
  add("ReaWeb_AudioWaveform", 1, 2, [this](const Json& a) { return controls_.host_call("ReaWeb_AudioWaveform", a); });
  add("ReaWeb_GetTrackMeter", 1, 1, [this](const Json& a) {
    auto track = native_->pointer(a[0], "MediaTrack");
    if (!track) throw Error("INVALID_HANDLE", "A track meter requires a live track handle");
    auto peak = reinterpret_cast<double (*)(void*, int)>(native_->resolve("Track_GetPeakInfo"));
    auto info = reinterpret_cast<double (*)(void*, const char*)>(native_->resolve("GetMediaTrackInfo_Value"));
    if (!peak || !info) throw Error("API_UNAVAILABLE", "Track meter APIs are unavailable");
    const double count = info(track, "I_NCHAN");
    if (!std::isfinite(count) || count < 1 || count > 128 || std::floor(count) != count)
      throw Error("AUDIO_UNSUPPORTED", "Invalid track channel count");
    Json values = Json::array(), db = Json::array();
    for (int channel = 0; channel < static_cast<int>(count); ++channel) {
      double value = peak(track, channel);
      if (!std::isfinite(value) || value < 0) throw Error("AUDIO_INVALID_DATA", "Invalid meter value");
      values.push_back(value); db.push_back(value > 0 ? Json(20 * std::log10(value)) : Json());
    }
    return Json{{"channels", static_cast<int>(count)}, {"peak", values}, {"peakDb", db}, {"unit", "linear-amplitude"}};
  });

}

Bridge::~Bridge() = default;

void Bridge::add(const std::string& name, size_t min, size_t max, std::function<Json(const Json&)> fn) {
  methods_.emplace(name, Method{min, max, std::move(fn)});
}

void Bridge::add_reaper(const std::string& name, std::function<Json(const Json&)> fn) {
  const auto& bindings = api_schema().at("bindings");
  if (!bindings.contains(name)) throw Error("SCHEMA_MISMATCH", "Unreviewed native binding: " + name);
  const auto& definition = bindings.at(name);
  add(name, definition.at("minArgs").get<size_t>(), definition.at("maxArgs").get<size_t>(), std::move(fn));
}

void Bridge::observe_project() {
  auto current = host_.current_project();
  project_ = current;
}
void Bridge::reset_handles() { native_->reset(); project_ = nullptr; }

Json parse_request(const std::string& message) {
    if (message.size() > message_limit) throw Error("MESSAGE_LIMIT", "Bridge message exceeds 64 MiB");
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
    if (method == methods_.end()) {
      const auto& known = api_schema().at("knownMethods");
      const auto& name = request.at("method");
      if (std::binary_search(known.begin(), known.end(), name))
        throw Error("SCHEMA_MISMATCH", "Native registry is missing a catalogued API", {{"method", name}});
      throw Error("UNKNOWN_API", "API is not registered in this runtime");
    }
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
