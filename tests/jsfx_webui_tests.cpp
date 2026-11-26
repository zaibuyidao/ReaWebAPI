#include "runtime/jsfx_webui.hpp"
#include "web/web_resources.hpp"
#include <cstring>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
using namespace reaweb;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
namespace {
struct Fx { Guid guid{}; double value = 0.25; std::string ident = "Utility/utility.jsfx"; };
std::vector<Fx> effects{{Guid{1}}, {Guid{2}}};
int writes = 0, ended = 0, selected = 0;
void* floating = nullptr;
struct Editor : JsfxEditorHost {
  std::optional<int> selection;
  void* parent() const override { return nullptr; }
  bool valid() const override { return true; }
  void resize(void*) override {}
  void present(void*) override {}
  void restore() override {}
  std::optional<int> selected_fx() const override { return selection; }
};
void* native(const char* name) {
#define FN(n, lambda) if (!std::strcmp(name, n)) return reinterpret_cast<void*>(+lambda)
  FN("TrackFX_GetCount", [](void*) { return int(effects.size()); });
  FN("TrackFX_GetFXGUID", [](void*, int fx) -> Guid* { return fx >= 0 && fx < int(effects.size()) ? &effects[fx].guid : nullptr; });
  FN("TrackFX_GetNamedConfigParm", [](void*, int fx, const char* key, char* out, int) {
    std::string value = !std::strcmp(key, "fx_type") ? "JS" : !std::strcmp(key, "fx_ident") ? effects.at(fx).ident : "0.5";
    std::strcpy(out, value.c_str()); return true;
  });
  FN("TrackFX_GetChainVisible", [](void*) { return selected; });
  FN("TrackFX_GetFloatingWindow", [](void*, int) { return floating; });
  FN("TrackFX_GetNumParams", [](void*, int) { return 1; });
  FN("TrackFX_GetParamEx", [](void*, int fx, int, double* lo, double* hi, double* mid) {
    *lo = 0; *hi = 1; *mid = 0.5; return effects.at(fx).value;
  });
  FN("TrackFX_GetParamNormalized", [](void*, int fx, int) { return effects.at(fx).value; });
  FN("TrackFX_GetParamName", [](void*, int, int, char* out, int) { std::strcpy(out, "Gain"); return true; });
  FN("TrackFX_GetParamIdent", [](void*, int, int, char* out, int) { std::strcpy(out, "0"); return true; });
  FN("TrackFX_GetFormattedParamValue", [](void*, int, int, char* out, int) { std::strcpy(out, "25%"); return true; });
  FN("TrackFX_GetParameterStepSizes", [](void*, int, int, double* step, double* small, double* large, bool* toggle) {
    *step = 0.001; *small = 0.001; *large = 0.1; *toggle = false; return true;
  });
  FN("TrackFX_SetParamNormalized", [](void*, int fx, int, double value) { ++writes; effects.at(fx).value = value; return true; });
  FN("TrackFX_SetParam", [](void*, int fx, int, double value) { ++writes; effects.at(fx).value = value; return true; });
  FN("TrackFX_EndParamEdit", [](void*, int, int) { ++ended; return true; });
#undef FN
  return nullptr;
}
template<class F> void rejects(F operation) { bool failed = false; try { operation(); } catch (const Error&) { failed = true; } CHECK(failed); }
}
int main() {
  try {
    int project = 0, other = 0, track = 0; bool valid = true; uint64_t generation = 0;
    void* current = &project;
    Host host; host.native_function = native;
    host.current_project = [&] { return current; };
    host.project_generation = [&] { return generation; };
    host.valid_track = [&](void* p, void* t) { return valid && p == &project && t == &track; };
    host.track_guid = [](void*) { return Guid{3}; };
    JsfxBinding binding(host, &track, 0);
    auto editor = std::make_shared<Editor>();
    JsfxEditor context{&track, 0, false, nullptr, editor};
    CHECK(binding.editor_matches(context));
    floating = &track;
    CHECK(!binding.editor_matches(context));
    auto floated = context; floated.floating = true; floated.container = floating;
    CHECK(binding.editor_matches(floated));
    floating = nullptr;
    CHECK(!binding.editor_matches(floated));
    CHECK(binding.editor_matches(context));
    editor->selection = 0; selected = 1;
    CHECK(binding.editor_matches(context));
    editor->selection = 1; selected = 0;
    CHECK(!binding.editor_matches(context));
    editor->selection = -1;
    CHECK(!binding.editor_matches(context));
    editor->selection.reset();
    const auto state = binding.state();
    CHECK(writes == 0 && state["parameters"][0]["value"] == 0.25);
    CHECK(state["parameters"][0]["defaultNormalized"] == 0.5);
    CHECK(state["parameters"][0]["enum"].is_null());
    CHECK(state["parameters"][0]["formatted"] == "25%");
    for (const auto* key : {"unit", "section", "reaweb"}) CHECK(!state["parameters"][0].contains(key));
    binding.set(0, 0.75, true); binding.end(0);
    CHECK(effects[0].value == 0.75 && effects[1].value == 0.25 && ended == 1);
    std::swap(effects[0], effects[1]);
    CHECK(binding.resolve() == 1);
    binding.set(0, 0.6, true); CHECK(effects[1].value == 0.6 && effects[0].value == 0.25);
    rejects([&] { binding.set(1, 0.5, true); });
    rejects([&] { binding.set(0, std::numeric_limits<double>::quiet_NaN(), true); });
    rejects([&] { binding.set(0, 2, true); }); CHECK(writes == 2);
    current = &other; rejects([&] { binding.set(0, 0.9, true); }); current = &project;
    ++generation; rejects([&] { binding.set(0, 0.9, true); }); --generation;
    valid = false; rejects([&] { binding.resolve(); }); valid = true;
    effects[1].ident = "Other/other.jsfx"; rejects([&] { binding.set(0, 0.9, true); });
    effects[1].ident = "Utility/utility.jsfx";
    effects.pop_back(); rejects([&] { binding.set(0, 0.9, true); }); CHECK(writes == 2);
    const auto root = fs::temp_directory_path() / ("reaweb-jsfx-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "Utility");
    const auto source = root / "Utility/utility.jsfx";
    std::ofstream(source) << "// metadata\ndesc:Utility EQ\ndesc:Second name\n@sample\nspl0 *= slider1;";
    std::ofstream(root / "Utility/index.html") << "<!doctype html>";
    const auto apps = root / "Apps";
    const std::string legacy = "jsfx-1aa152a394a48d8b-0";
    const std::string named = "jsfx-utility-eq-1aa152a394a48d8b";
    const auto open = [&](const fs::path& profiles) {
      return open_app_binding(root / "Utility", profiles, "index.html", legacy, {}, jsfx_display_name(source));
    };
    CHECK(open(apps).id == named);
    CHECK(Json::parse(std::ifstream(apps / named / "origin.json")).at("displayName") == "Utility EQ");
    std::ofstream(source) << "desc:Renamed\n";
    CHECK(open(apps).id == named);
    CHECK(Json::parse(std::ifstream(apps / named / "origin.json")).at("displayName") == "Renamed");
    const auto old_apps = root / "OldApps";
    fs::create_directories(old_apps / legacy / "Data");
    CHECK(open(old_apps).id == legacy);
    CHECK(fs::exists(old_apps / legacy / "origin.json") && !fs::exists(old_apps / named));
    CHECK(!jsfx_association(root, "Utility/utility.jsfx"));
    std::ofstream(root / "Utility/utility.json") << R"({"schema":1,"JSFX_WebUI":{"entry":"index.html"}})";
    CHECK(jsfx_association(root, "Utility/utility.jsfx").has_value());
    CHECK(jsfx_association(root, "Utility/utility.jsfx")->daw_keyboard);
    for (const auto value : {Json(true), Json(false), Json(), Json(0), Json("false"), Json::array(), Json::object()}) {
      std::ofstream(root / "Utility/utility.json") << Json{{"schema", 1}, {"JSFX_WebUI", {{"entry", "index.html"}, {"dawKeyboard", value}}}};
      const auto association = jsfx_association(root, "Utility/utility.jsfx");
      CHECK(bool(association) == value.is_boolean());
      if (association) CHECK(association->daw_keyboard == value.get<bool>());
    }
    std::ofstream(root / "Utility/utility.json") << R"({"schema":1,"JSFX_WebUI":{"entry":"../outside.html"}})";
    std::ofstream(root / "outside.html") << "<!doctype html>";
    CHECK(!jsfx_association(root, "Utility/utility.jsfx"));
    CHECK(!jsfx_association(root, "../utility.jsfx"));
    fs::remove_all(root);
    std::cout << "JSFX identity, parameter and association tests passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
