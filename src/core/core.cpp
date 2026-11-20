#include "core/core.hpp"
#include "runtime/services.hpp"
#include "runtime/file_writer.hpp"
#include <regex>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace reaweb {
namespace {
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

Bridge::Bridge(Host& host, Controls controls)
  : controls_(std::move(controls)), host_(host) {
  add("ReaWeb_Open", 1, 1, [this](const Json& a) { return controls_.open(string_arg(a[0])); });
  add("ReaWeb_OpenEx", 2, 2, [this](const Json& a) {
    string_arg(a[0]);
    if (!a[1].is_object()) throw Error("INVALID_ARGUMENT", "Expected window open options");
    for (const auto& field : a[1].items())
      if ((field.key() == "layer" || field.key() == "mouseMode") ? !field.value().is_string() :
          field.key() == "mouseRegions" ? !field.value().is_array() :
          (field.key() != "hidden" && field.key() != "frameless" && field.key() != "resizable" && field.key() != "transparent") || !field.value().is_boolean())
        throw Error("INVALID_ARGUMENT", "Unknown or invalid window open option: " + field.key());
    return controls_.host_call("ReaWeb_OpenEx", a);
  });
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
    return Json{{"version", REAWEB_VERSION}, {"protocol", 1}, {"methods", names}, {"projectScope", "all"},
      {"events", runtime_events()},
      {"runtime", {{"contract", 2}, {"namespaces", {"window", "theme", "dialog", "events", "lifecycle", "debug", "fs", "audio",
                                                  "clipboard", "dragDrop", "app", "system", "transaction", "host", "stream", "realtime", "video"}},
        {"host", {{"maxMessageBytes", host_message_limit}, {"maxPendingMessages", host_queue_limit}, {"maxQueuedBytes", host_queue_bytes},
          {"serviceABI", 1}, {"serviceTimeoutMs", 30000}, {"builtinServices", {"runtime"}}}},
        {"window", {{"hiddenStart", true}}},
        {"fs", {{"streamingWrite", {{"maxChunkBytes", FileWriters::chunk_limit}, {"maxFileBytes", FileWriters::file_limit}, {"maxWriters", 4}, {"maxWritersPerDocument", 1}, {"idleTimeoutMs", 300000}}}, {"mediaURLs", true}, {"maxOpenMedia", 32}, {"maxMediaBytes", 64ull * 1024 * 1024 * 1024}}},
        {"reservedNamespaces", Json::array()},
        {"stream", {{"abi", 1}, {"transport", "websocket-binary"}, {"maxStreams", 128}, {"maxConsumers", 64}, {"maxBytes", 16 * 1024 * 1024},
          {"kinds", {"frame", "audio", "spectrum", "meter", "waveform", "binary", "midi"}}}},
        {"dragDrop", {{"maxFiles", 256}, {"maxTextBytes", value_limit}, {"effect", "copy"}}},
        {"cleanupTimeoutMs", 2000}, {"audio", {{"maxChannels", 32}, {"maxWaveformPoints", 8192}, {"maxPendingJobs", 8}}}}},
      {"limits", {{"requestBytes", message_limit}, {"pendingCalls", 256}}}};
  });
  for (const auto* name : {"ReaWeb_StreamOpen", "ReaWeb_StreamDetach"})
    add(name, 1, 1, [this, name](const Json& a) { return controls_.host_call(name, a); });
  add("ReaWeb_StreamDiagnostics", 0, 0, [this](const Json& a) { return controls_.host_call("ReaWeb_StreamDiagnostics", a); });
  for (const auto* name : {"ReaWeb_VideoCapabilities", "ReaWeb_VideoSourceInfo", "ReaWeb_VideoState", "ReaWeb_VideoDiagnostics"})
    add(name, 0, 0, [this, name](const Json& a) { return controls_.host_call(name, a); });
  add("ReaWeb_VideoOpen", 1, 1, [this](const Json& a) { return controls_.host_call("ReaWeb_VideoOpen", a); });
  for (const auto* name : {"ReaWeb_VideoSurfaceOpen", "ReaWeb_VideoSurfaceDiagnostics", "ReaWeb_VideoSurfaceClose"})
    add(name, 1, 1, [this, name](const Json& a) { return controls_.host_call(name, a); });
  add("ReaWeb_VideoSurfaceLayout", 2, 2, [this](const Json& a) { return controls_.host_call("ReaWeb_VideoSurfaceLayout", a); });
  for (const auto* name : {"ReaWeb_RealtimeCapabilities", "ReaWeb_RealtimeDiagnostics"})
    add(name, 0, 0, [this, name](const Json& a) { return controls_.host_call(name, a); });
  add("ReaWeb_RealtimeSubscribe", 2, 2, [this](const Json& a) { return controls_.host_call("ReaWeb_RealtimeSubscribe", a); });
  add("ReaWeb_RealtimeUnsubscribe", 1, 1, [this](const Json& a) { return controls_.host_call("ReaWeb_RealtimeUnsubscribe", a); });
  add("ReaWeb_WatchBegin", 2, 2, [this](const Json& a) { return controls_.host_call("ReaWeb_WatchBegin", a); });
  add("ReaWeb_AnalysisOpen", 2, 2, [this](const Json& a) { return controls_.host_call("ReaWeb_AnalysisOpen", a); });
  add("ReaWeb_MeterReset", 1, 1, [this](const Json& a) { return controls_.host_call("ReaWeb_MeterReset", a); });
  add("ReaWeb_MIDIOpen", 1, 1, [this](const Json& a) { return controls_.host_call("ReaWeb_MIDIOpen", a); });
  add("ReaWeb_GetDevices", 0, 0, [this](const Json& a) { return controls_.host_call("ReaWeb_GetDevices", a); });
  add("ReaWeb_GetDisplays", 0, 0, [this](const Json& a) { return controls_.host_call("ReaWeb_GetDisplays", a); });
  add("ReaWeb_ClipboardReadBinary", 1, 1, [this](const Json& a) { return controls_.host_call("ReaWeb_ClipboardReadBinary", a); });
  add("ReaWeb_ClipboardWriteBinary", 2, 2, [this](const Json& a) { return controls_.host_call("ReaWeb_ClipboardWriteBinary", a); });
  for (const auto* name : {"ReaWeb_WatchEnd", "ReaWeb_TimerStart", "ReaWeb_TimerStop"})
    add(name, 1, 1, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto* name : {"ReaWeb_ServiceInvoke", "ReaWeb_ServiceSend"})
    add(name, 3, 3, [](const Json&) -> Json { throw Error("MAIN_THREAD_REQUIRED", "Host Service requires Runtime dispatch"); });
  for (const auto* name : {"ReaWeb_ServiceSubscribe", "ReaWeb_ServiceUnsubscribe"})
    add(name, 2, 2, [](const Json&) -> Json { throw Error("MAIN_THREAD_REQUIRED", "Host Service requires Runtime dispatch"); });
  for (const auto& name : {"ReaWeb_GetWindowState", "ReaWeb_GetDiagnostics", "ReaWeb_Focus"})
    add(name, 0, 0, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto& name : {"ReaWeb_SetShortcutRouting", "ReaWeb_SetMouseMode", "ReaWeb_SetMouseRegions", "ReaWeb_SetTitle", "ReaWeb_SetIcon", "ReaWeb_SetIconVisible", "ReaWeb_SetFrameless", "ReaWeb_SetCornerRadius", "ReaWeb_SetKeyboardCapture", "ReaWeb_SetDawKeyboard", "ReaWeb_Subscribe", "ReaWeb_Unsubscribe"})
    add(name, 1, 1, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto& name : {"ReaWeb_HostSend", "ReaWeb_OpenDev", "ReaWeb_BeginUndo", "ReaWeb_EndUndo", "ReaWeb_ClipboardWriteText", "ReaWeb_OpenExternal"})
    add(name, 1, 1, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto& name : {"ReaWeb_ClipboardReadText"})
    add(name, 0, 0, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto& name : {"ReaWeb_ReadFile", "ReaWeb_ReadDirectory", "ReaWeb_Stat", "ReaWeb_MakeDirectory"})
    add(name, 1, 2, [this, name](const Json& a) { return controls_.host_call(name, a); });
  add("ReaWeb_WriteFile", 2, 3, [this](const Json& a) { return controls_.host_call("ReaWeb_WriteFile", a); });
  add("ReaWeb_OpenWrite", 1, 2, [this](const Json& a) { return controls_.host_call("ReaWeb_OpenWrite", a); });
  add("ReaWeb_WriteChunk", 3, 3, [this](const Json& a) { return controls_.host_call("ReaWeb_WriteChunk", a); });
  for (const auto* name : {"ReaWeb_CommitWrite", "ReaWeb_AbortWrite", "ReaWeb_WriteStatus"})
    add(name, 1, 1, [this, name](const Json& a) { return controls_.host_call(name, a); });
  add("ReaWeb_MoveFile", 2, 3, [this](const Json& a) { return controls_.host_call("ReaWeb_MoveFile", a); });
  for (const auto* name : {"ReaWeb_OpenMedia", "ReaWeb_CloseMedia"})
    add(name, 1, 1, [](const Json&) -> Json { throw Error("HOST_UNAVAILABLE", "Media URLs require a local App document"); });
  for (const auto& name : {"ReaWeb_GetBounds", "ReaWeb_GetTheme", "ReaWeb_GetLogs", "ReaWeb_Reload", "ReaWeb_GetAppInfo", "ReaWeb_GetPlatform", "ReaWeb_GetArchitecture"})
    add(name, 0, 0, [this, name](const Json& a) { return controls_.host_call(name, a); });
  for (const auto& name : {"ReaWeb_SetBounds", "ReaWeb_SetVisible", "ReaWeb_Log", "ReaWeb_LifecycleSubscribe", "ReaWeb_LifecycleComplete", "ReaWeb_AudioFileInfo", "ReaWeb_DragFiles", "ReaWeb_DragText", "ReaWeb_RevealPath"})
    add(name, 1, 1, [this, name](const Json& a) { return controls_.host_call(name, a); });
  add("ReaWeb_AudioWaveform", 1, 2, [this](const Json& a) { return controls_.host_call("ReaWeb_AudioWaveform", a); });
  add_runtime_calls();

}

Bridge::~Bridge() = default;

void Bridge::add(const std::string& name, size_t min, size_t max, std::function<Json(const Json&)> fn) {
  methods_.emplace(name, Method{min, max, std::move(fn)});
}

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
      throw Error("UNKNOWN_API", "API is not registered in this runtime");
    }
    const auto& args = request["args"];
    if (args.size() < method->second.min_args || args.size() > method->second.max_args)
      throw Error("INVALID_ARGUMENT", "Wrong argument count");
    project_ = host_.current_project();
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
