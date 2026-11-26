#include "runtime/jsfx_webui.hpp"
#include <cmath>
#include <climits>
#include <algorithm>
#include <cstring>
#include <fstream>

namespace reaweb {
namespace {
constexpr int max_parameters = 259; // 256 JSFX sliders plus REAPER's Bypass, Wet and Delta.
template<class T> T api(Host& host, const char* name) {
  auto result = host.native_function ? reinterpret_cast<T>(host.native_function(name)) : nullptr;
  if (!result) throw Error("HOST_UNAVAILABLE", std::string("JSFX Web UI requires ") + name);
  return result;
}
bool beneath(const fs::path& root, const fs::path& path) {
  auto relative = path.lexically_relative(root);
  return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
Guid object_guid(Host& host, void* object, const char* function) {
  auto guid = api<void* (*)(void*, const char*, void*)>(host, function)(object, "GUID", nullptr);
  if (!guid) throw Error("FX_STALE", "JSFX owner identity is unavailable");
  return *static_cast<Guid*>(guid);
}
}
std::string JsfxTarget::config(const Host& host, int fx, const std::string& key) const {
  char buffer[4096]{};
  return api<bool (*)(void*, int, const char*, char*, int)>(host, "GetNamedConfigParm")(
    object(), fx, key.c_str(), buffer, sizeof(buffer)) ? buffer : "";
}
int JsfxTarget::selected(Host& host) const {
  const int value = api<int (*)(void*)>(host, input ? "GetRecChainVisible" : "GetChainVisible")(object());
  return value >= 0 && input ? value | 0x1000000 : value;
}
std::vector<int> JsfxTarget::indices(const Host& host) const {
  const int count = api<int (*)(void*)>(host, input ? "GetRecCount" : "GetCount")(object());
  std::vector<int> result, pending;
  for (int i = std::min(count, 4096) - 1; i >= 0; --i) pending.push_back(i | (input ? 0x1000000 : 0));
  while (!pending.empty() && result.size() < 4096) {
    const auto fx = pending.back(); pending.pop_back();
    if (std::find(result.begin(), result.end(), fx) != result.end()) continue;
    result.push_back(fx);
    const auto children = config(host, fx, "container_count");
    char* end = nullptr;
    const auto n = std::strtol(children.c_str(), &end, 10);
    if (children.empty() || !end || *end || n < 0 || n > 4096) continue;
    for (int i = int(n) - 1; i >= 0 && pending.size() < 4096; --i) {
      const auto child = config(host, fx, "container_item." + std::to_string(i));
      const auto index = std::strtol(child.c_str(), &end, 10);
      if (!child.empty() && end && !*end && index >= 0x2000000 && index <= INT_MAX &&
          (take || bool(index & 0x1000000) == input)) pending.push_back(int(index));
    }
  }
  return result;
}
std::string jsfx_display_name(const fs::path& source) {
  std::ifstream input(source, std::ios::binary);
  std::string header(65536, '\0');
  input.read(header.data(), static_cast<std::streamsize>(header.size()));
  header.resize(static_cast<size_t>(input.gcount()));
  size_t start = header.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  while (start < header.size()) {
    const auto end = header.find_first_of("\r\n", start);
    if (end == std::string::npos && !input.eof()) break;
    const auto line = header.substr(start, end - start);
    const auto first = line.find_first_not_of(" \t");
    if (first != std::string::npos) {
      if (line[first] == '@') break;
      if (line.compare(first, 5, "desc:") == 0) {
        const auto name = line.find_first_not_of(" \t", first + 5);
        if (name == std::string::npos) break;
        const auto value = line.substr(name, line.find_last_not_of(" \t") - name + 1);
        if (value.size() <= 1024 && value.find('\0') == std::string::npos) {
          try { Json(value).dump(); return value; } catch (const Json::type_error&) {}
        }
        break;
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}
std::optional<JsfxAssociation> jsfx_association(const fs::path& effects, const std::string& ident) {
  if (ident.empty() || ident.find('\0') != std::string::npos) return {};
  std::error_code error;
  const auto root = fs::weakly_canonical(effects, error);
  if (error) return {};
  auto source = fs::weakly_canonical(root / fs::u8path(ident), error);
  if (error || !beneath(root, source) || !fs::is_regular_file(source)) return {};
  auto sidecar = source; sidecar.replace_extension(".json");
  if (!fs::is_regular_file(sidecar) || fs::file_size(sidecar) > 65536) return {};
  std::ifstream input(sidecar);
  auto data = Json::parse(input, nullptr, false);
  if (!data.is_object() || data.value("schema", Json()) != 1 || !data.contains("JSFX_WebUI")) return {};
  const auto& ui = data["JSFX_WebUI"];
  if (!ui.is_object() || !ui.value("entry", Json()).is_string() || ui.value("enabled", Json(true)) != true) return {};
  const auto daw_keyboard = ui.value("dawKeyboard", Json(true));
  if (!daw_keyboard.is_boolean()) return {};
  auto entry = fs::weakly_canonical(source.parent_path() / fs::u8path(ui["entry"].get<std::string>()), error);
  if (error || !beneath(source.parent_path(), entry) || !fs::is_regular_file(entry) || entry.extension() != ".html") return {};
  const auto width = ui.value("width", Json(640)), height = ui.value("height", Json(420));
  if (!width.is_number_integer() || !height.is_number_integer() || width.get<double>() < 100 || width.get<double>() > 2048 ||
      height.get<double>() < 100 || height.get<double>() > 2048) return {};
  return JsfxAssociation{entry, ident, width.get<int>(), height.get<int>(), ui.value("native", Json()), daw_keyboard.get<bool>()};
}
JsfxBinding::JsfxBinding(Host& host, void* track, int fx)
  : JsfxBinding(host, JsfxTarget{track, nullptr, bool(fx & 0x1000000)}, fx) {}
JsfxBinding::JsfxBinding(Host& host, JsfxTarget target, int fx)
  : host_(host), project_(host.current_project()), target_(target), index_(fx) {
  if (!host_.valid_track(project_, target_.track) || fx < 0 || (target_.take && target_.input))
    throw Error("FX_STALE", "JSFX instance is no longer available");
  track_guid_ = host_.track_guid(target_.track);
  generation_ = host_.project_generation ? host_.project_generation() : 0;
  if (target_.take) {
    if (!api<bool (*)(void*, void*, const char*)>(host_, "ValidatePtr2")(project_, target_.take, "MediaItem_Take*"))
      throw Error("FX_STALE", "JSFX take is no longer available");
    item_ = api<void* (*)(void*)>(host_, "GetMediaItemTake_Item")(target_.take);
    item_guid_ = object_guid(host_, item_, "GetSetMediaItemInfo");
    take_guid_ = object_guid(host_, target_.take, "GetSetMediaItemTakeInfo");
  }
  validate_owner();
  const auto indices = target_.indices(host_);
  if (std::find(indices.begin(), indices.end(), fx) == indices.end())
    throw Error("FX_STALE", "JSFX chain address is no longer available");
  const auto guid = target_.api<Guid* (*)(void*, int)>(host_, "GetFXGUID")(target_.object(), index_);
  if (!guid || target_.config(host_, index_, "fx_type") != "JS") throw Error("FX_STALE", "Expected a native JSFX instance");
  fx_guid_ = *guid;
  ident_ = target_.config(host_, index_, "fx_ident");
}
void JsfxBinding::validate_owner() {
  if (host_.current_project() != project_ || (host_.project_generation && host_.project_generation() != generation_) ||
      !host_.valid_track(project_, target_.track) || host_.track_guid(target_.track) != track_guid_)
    throw Error("FX_STALE", "JSFX project or track identity changed");
  if (!target_.take) return;
  const auto valid = api<bool (*)(void*, void*, const char*)>(host_, "ValidatePtr2");
  if (!valid(project_, item_, "MediaItem*") || !valid(project_, target_.take, "MediaItem_Take*") ||
      object_guid(host_, item_, "GetSetMediaItemInfo") != item_guid_ ||
      object_guid(host_, target_.take, "GetSetMediaItemTakeInfo") != take_guid_ ||
      api<void* (*)(void*)>(host_, "GetMediaItemTake_Item")(target_.take) != item_ ||
      api<void* (*)(void*)>(host_, "GetMediaItem_Track")(item_) != target_.track)
    throw Error("FX_STALE", "JSFX item or take identity changed");
}
int JsfxBinding::resolve() {
  validate_owner();
  const auto guid = target_.api<Guid* (*)(void*, int)>(host_, "GetFXGUID");
  const auto matches = [&](int i) { auto g = guid(target_.object(), i); return g && *g == fx_guid_; };
  if (index_ < 0 || !matches(index_)) {
    index_ = -1;
    for (int i : target_.indices(host_)) if (matches(i)) { index_ = i; break; }
  }
  if (index_ < 0 || target_.config(host_, index_, "fx_type") != "JS" || target_.config(host_, index_, "fx_ident") != ident_)
    throw Error("FX_STALE", "JSFX instance was removed or replaced");
  return index_;
}
bool JsfxBinding::editor_matches(const JsfxEditor& editor) {
  const auto fx = resolve();
  if (editor.target() != target_ || !editor.host || !editor.host->valid()) return false;
  if (!editor.floating) {
    if (target_.api<void* (*)(void*, int)>(host_, "GetFloatingWindow")(target_.object(), fx)) return false;
    const auto selected = editor.host->selected_fx();
    const bool matches = (selected ? *selected : target_.selected(host_)) == fx;
    if (matches) editor.host->sync_selection(fx);
    return matches;
  }
  return target_.api<void* (*)(void*, int)>(host_, "GetFloatingWindow")(target_.object(), fx) == editor.container;
}
std::string JsfxBinding::ident() { return target_.config(host_, resolve(), "fx_ident"); }
int JsfxBinding::parameter_count() {
  const int count = target_.api<int (*)(void*, int)>(host_, "GetNumParams")(target_.object(), resolve());
  if (count < 0 || count > max_parameters) throw Error("FX_LIMIT", "JSFX Web UI supports at most 259 host parameters");
  return count;
}
Json JsfxBinding::parameter(int param, bool metadata) {
  const auto fx = resolve();
  const auto count = target_.api<int (*)(void*, int)>(host_, "GetNumParams")(target_.object(), fx);
  if (param < 0 || param >= count || count > max_parameters) throw Error("INVALID_ARGUMENT", "Invalid JSFX parameter index");
  double minimum = 0, maximum = 0, midpoint = 0;
  const auto value = target_.api<double (*)(void*, int, int, double*, double*, double*)>(host_, "GetParamEx")(
    target_.object(), fx, param, &minimum, &maximum, &midpoint);
  const auto normalized = target_.api<double (*)(void*, int, int)>(host_, "GetParamNormalized")(target_.object(), fx, param);
  char text[1024]{};
  const auto string = [&](const char* name) -> Json {
    text[0] = 0;
    return target_.api<bool (*)(void*, int, int, char*, int)>(host_, name)(target_.object(), fx, param, text, sizeof(text)) ? Json(text) : Json();
  };
  Json result{{"index", param}, {"value", value}, {"normalized", normalized}, {"formatted", string("GetFormattedParamValue")}};
  if (!metadata) return result;
  result.update({{"name", string("GetParamName")}, {"ident", string("GetParamIdent")},
    {"min", minimum}, {"max", maximum}, {"mid", midpoint}, {"defaultNormalized", nullptr},
    {"step", nullptr}, {"smallStep", nullptr}, {"largeStep", nullptr}, {"toggle", nullptr}, {"enum", nullptr}});
  auto default_value = target_.config(host_, fx, "param." + std::to_string(param) + ".default_value");
  if (!default_value.empty()) {
    char* end = nullptr; const auto d = std::strtod(default_value.c_str(), &end);
    if (end && !*end && std::isfinite(d) && d >= 0 && d <= 1) result["defaultNormalized"] = d;
  }
  double step = 0, small = 0, large = 0; bool toggle = false;
  if (target_.api<bool (*)(void*, int, int, double*, double*, double*, bool*)>(host_, "GetParameterStepSizes")(
      target_.object(), fx, param, &step, &small, &large, &toggle))
    result.update({{"step", step}, {"smallStep", small}, {"largeStep", large}, {"toggle", toggle}});
  return result;
}
Json JsfxBinding::state() {
  Json parameters = Json::array();
  const auto count = parameter_count();
  for (int p = 0; p < count; ++p) parameters.push_back(parameter(p, true));
  return {{"parameters", parameters}, {"count", count}, {"fxIdent", ident()}};
}
void JsfxBinding::set(int param, double value, bool normalized) {
  auto state = parameter(param, true);
  const auto minimum = normalized ? 0.0 : state["min"].get<double>(), maximum = normalized ? 1.0 : state["max"].get<double>();
  if (!std::isfinite(value) || value < minimum || value > maximum) throw Error("INVALID_ARGUMENT", "JSFX parameter value is outside its range");
  if (!target_.api<bool (*)(void*, int, int, double)>(host_, normalized ? "SetParamNormalized" : "SetParam")(
      target_.object(), resolve(), param, value)) throw Error("FX_WRITE_FAILED", "REAPER rejected the parameter value");
}
void JsfxBinding::end(int param) {
  const auto fx = resolve();
  if (param >= 0 && param < parameter_count()) target_.api<bool (*)(void*, int, int)>(host_, "EndParamEdit")(target_.object(), fx, param);
}
}
