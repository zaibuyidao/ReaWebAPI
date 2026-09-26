#include "runtime/native_monitor.hpp"
#include <cstdio>

namespace reaweb {
void NativeMonitorManager::add(std::string name, Read read, std::function<uint64_t()> revision, bool invalidate, std::function<void()> reset) {
  Monitor monitor;
  monitor.read = std::move(read); monitor.source_revision = std::move(revision); monitor.invalidate = invalidate;
  monitor.reset = std::move(reset);
  monitors_.emplace(std::move(name), std::move(monitor));
}
bool NativeMonitorManager::contains(const std::string& name) const { return monitors_.count(name) != 0; }
Json NativeMonitorManager::snapshot(const std::string& name) const { return monitors_.at(name).event; }
void NativeMonitorManager::subscriptions(const std::map<std::string, size_t>& counts) {
  for (auto& item : monitors_) {
    auto& m = item.second;
    const auto found = counts.find(item.first);
    const auto count = found == counts.end() ? 0 : found->second;
    if ((!count != !m.subscribers) && m.reset) m.reset();
    if (!count || !m.subscribers) { m.previous = m.event = nullptr; m.next = {}; m.epoch = 0; }
    m.subscribers = count;
  }
}
void NativeMonitorManager::tick(uint64_t epoch, Clock::time_point deadline, const Emit& emit) {
  for (size_t n = 0; n < monitors_.size() && Clock::now() < deadline; ++n) {
    auto it = monitors_.upper_bound(cursor_);
    if (it == monitors_.end()) it = monitors_.begin();
    cursor_ = it->first;
    auto& m = it->second;
    if (!m.subscribers) continue;
    const auto revision = m.source_revision ? m.source_revision() : 0;
    if (Clock::now() < m.next && revision == m.source && epoch == m.epoch) continue;
    auto state = m.read(deadline);
    if (!state) { m.next = {}; continue; }
    m.source = revision; m.next = Clock::now() + std::chrono::milliseconds(100);
    if (epoch == m.epoch && *state == m.previous) continue;
    m.epoch = epoch; m.previous = *state;
    m.event = m.invalidate ? Json{{"invalidated", true}, {"count", state->value("count", 0)},
      {"available", state->value("available", true)}} : *state;
    m.event["projectEpoch"] = epoch; m.event["revision"] = ++m.revision;
    emit(it->first, m.event);
  }
}
namespace {
template<class T> T api(const Host& host, const char* name) {
  return host.native_function ? reinterpret_cast<T>(host.native_function(name)) : nullptr;
}
std::string identity(const Guid& value) {
  std::string text;
  const char* hex = "0123456789abcdef";
  for (auto c : value) { text += hex[c >> 4]; text += hex[c & 15]; }
  return text;
}
struct Scan {
  const Host& host;
  std::string kind;
  void* project = nullptr;
  uint64_t generation = 0, revision = 0;
  int changes = -1, index = 0, count = -1;
  Json values = Json::array();
  std::optional<Json> read(NativeMonitorManager::Clock::time_point deadline) {
    auto p = host.current_project();
    const auto g = host.project_generation ? host.project_generation() : 0;
    const auto c = host.change_count ? host.change_count(p) : 0;
    const bool selection = kind == "trackSelectionChanged", tracks = kind == "trackStateChanged";
    const auto r = host.event_revision ? host.event_revision(selection || tracks ? "track-selected" : "marker-changed") : 0;
    auto count_tracks = api<int (*)(void*)>(host, "CountTracks");
    auto get_track = api<void* (*)(void*, int)>(host, "GetTrack");
    auto marker_count = api<int (*)(void*, int*, int*)>(host, "CountProjectMarkers");
    auto marker = api<int (*)(void*, int, bool*, double*, double*, const char**, int*, int*)>(host, "EnumProjectMarkers3");
    if ((tracks && (!count_tracks || !get_track)) || (!selection && !tracks && (!marker_count || !marker)))
      return Json{{"available", false}};
    int nm = 0, nr = 0;
    const int total = selection ? host.count_selected_tracks(p) : tracks ? count_tracks(p) : marker_count(p, &nm, &nr);
    if (project != p || generation != g || changes != c || revision != r || count != total || index == 0) {
      project = p; generation = g; changes = c; revision = r; count = total; index = 0; values = Json::array();
    }
    auto track_name = api<bool (*)(void*, char*, int)>(host, "GetTrackName");
    auto track_value = api<double (*)(void*, const char*)>(host, "GetMediaTrackInfo_Value");
    auto track_color = api<int (*)(void*)>(host, "GetTrackColor");
    for (int n = 0; n < 64 && index < count && NativeMonitorManager::Clock::now() < deadline; ++n, ++index) {
      if (selection || tracks) {
        auto track = selection ? host.get_selected_track(p, index) : get_track(p, index);
        if (!track || !host.valid_track(p, track)) { index = 0; return std::nullopt; }
        Json data{{"guid", identity(host.track_guid(track))}};
        if (tracks) {
          char name[4096]{};
          if (track_name && track_name(track, name, sizeof(name))) data["name"] = name;
          if (track_color) data["color"] = track_color(track);
          if (track_value) for (const char* key : {"B_MUTE", "I_SOLO", "I_RECARM", "B_SHOWINTCP", "B_SHOWINMIXER"}) data[key] = track_value(track, key);
        }
        values.push_back(std::move(data));
      } else {
        bool region = false; double start = 0, end = 0; const char* name = nullptr; int id = 0, color = 0;
        if (!marker(p, index, &region, &start, &end, &name, &id, &color)) { index = 0; return std::nullopt; }
        if (region == (kind == "regionsChanged")) values.push_back({{"id", id}, {"start", start}, {"end", end}, {"name", name ? name : ""}, {"color", color}});
      }
    }
    if (index < count) return std::nullopt;
    index = 0;
    return Json{{"available", true}, {"count", values.size()}, {"items", std::move(values)}};
  }
};
Json transport(const Host& host) {
  auto state = api<int (*)()>(host, "GetPlayState");
  auto play = api<double (*)()>(host, "GetPlayPosition");
  auto cursor = api<double (*)()>(host, "GetCursorPosition");
  auto rate = api<double (*)(void*)>(host, "Master_GetPlayRate");
  auto repeat = api<int (*)(int)>(host, "GetSetRepeat");
  if (!state || !play || !cursor || !rate || !repeat) return {{"available", false}};
  const int flags = state();
  return {{"available", true}, {"state", flags}, {"playing", (flags & 1) != 0}, {"paused", (flags & 2) != 0},
    {"recording", (flags & 4) != 0}, {"position", play()}, {"cursor", cursor()}, {"rate", rate(host.current_project())}, {"loop", repeat(-1) != 0}};
}
Json projects(const Host& host) {
  auto enumerate = api<void* (*)(int, char*, int)>(host, "EnumProjects");
  auto dirty = api<int (*)(void*)>(host, "IsProjectDirty");
  if (!enumerate || !dirty) return {{"available", false}};
  Json projects = Json::array();
  std::string active;
  auto current = host.current_project();
  for (int i = 0; ; ++i) {
    char path[32768]{};
    auto project = enumerate(i, path, sizeof(path));
    if (!project) break;
    const auto id = std::to_string(reinterpret_cast<uintptr_t>(project));
    if (project == current) active = id;
    projects.push_back({{"id", id}, {"path", path}, {"dirty", dirty(project) != 0}});
  }
  return {{"available", true}, {"activeProject", active}, {"projects", projects},
    {"changeCount", host.change_count ? host.change_count(current) : 0},
    {"generation", host.project_generation ? host.project_generation() : 0}};
}
Json range(const Host& host, bool loop) {
  auto get = api<void (*)(void*, bool, bool, double*, double*, bool)>(host, "GetSet_LoopTimeRange2");
  if (!get) return {{"available", false}};
  double start = 0, end = 0; get(host.current_project(), false, loop, &start, &end, false);
  return {{"available", true}, {"start", start}, {"end", end}};
}
Json current_region(const Host& host) {
  auto current = api<void (*)(void*, double, int*, int*)>(host, "GetLastMarkerAndCurRegion");
  auto marker = api<int (*)(void*, int, bool*, double*, double*, const char**, int*, int*)>(host, "EnumProjectMarkers3");
  auto state = api<int (*)()>(host, "GetPlayState");
  auto play = api<double (*)()>(host, "GetPlayPosition");
  auto cursor = api<double (*)()>(host, "GetCursorPosition");
  if (!current || !marker || !state || !play || !cursor) return {{"available", false}};
  int m = -1, r = -1; auto project = host.current_project();
  current(project, (state() & 1) ? play() : cursor(), &m, &r);
  Json value = nullptr;
  if (r >= 0) {
    bool region = false; double start = 0, end = 0; const char* name = nullptr; int id = 0, color = 0;
    if (marker(project, r, &region, &start, &end, &name, &id, &color) && region)
      value = {{"id", id}, {"start", start}, {"end", end}, {"name", name ? name : ""}, {"color", color}};
  }
  return {{"available", true}, {"region", value}};
}
}
void register_native_monitors(NativeMonitorManager& manager, const Host& host) {
  auto revision = [&host](const std::string& name) { return [&host, name] { return host.event_revision ? host.event_revision(name) : 0; }; };
  for (const char* name : {"trackSelectionChanged", "trackStateChanged", "markersChanged", "regionsChanged"}) {
    auto scan = std::make_shared<Scan>(Scan{host, name});
    manager.add(name, [scan](auto deadline) { return scan->read(deadline); },
      revision(std::string(name).rfind("track", 0) == 0 ? "track-selected" : "marker-changed"), true,
      [scan] { scan->index = 0; scan->values = Json::array(); });
  }
  manager.add("transportChanged", [&host](auto) { return transport(host); }, revision("transport"));
  manager.add("projectChanged", [&host](auto) { return projects(host); });
  manager.add("loopPointsChanged", [&host](auto) { return range(host, true); });
  manager.add("timeSelectionChanged", [&host](auto) { return range(host, false); });
  manager.add("currentRegionChanged", [&host](auto) { return current_region(host); }, revision("marker-changed"));
}
}
