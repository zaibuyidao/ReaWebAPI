#include "runtime/native_monitor.hpp"
#include <cstring>
#include <iostream>
#include <thread>
using namespace reaweb;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
namespace {
std::vector<int> selection;
int changes = 0, state = 0, dirty = 0, color = 0;
bool extra_project = false;
double position = 0, rate = 1, end = 1;
std::string marker_name = "marker", region_name = "region", project_path = "project.rpp";
void* resolve(const char* name) {
#define FN(n, f) if (!std::strcmp(name,n)) return reinterpret_cast<void*>(+f)
  FN("CountTracks", [](void*) { return 3; });
  FN("GetTrack", [](void*,int i) { return reinterpret_cast<void*>(uintptr_t(i+1)); });
  FN("GetTrackName", [](void*,char* name,int) { std::strcpy(name,"track"); return true; });
  FN("GetTrackColor", [](void*) { return color; });
  FN("GetMediaTrackInfo_Value", [](void*,const char*) { return 0.; });
  FN("GetPlayState", [] { return state; });
  FN("GetPlayPosition", [] { return position; });
  FN("GetCursorPosition", [] { return 0.; });
  FN("Master_GetPlayRate", [](void*) { return rate; });
  FN("GetSetRepeat", [](int) { return 0; });
  FN("EnumProjects", [](int i,char* path,int) -> void* {
    if (i > (extra_project ? 1 : 0)) return nullptr;
    if (path) std::strcpy(path,project_path.c_str());
    return reinterpret_cast<void*>(uintptr_t(i+1));
  });
  FN("IsProjectDirty", [](void*) { return dirty; });
  FN("GetSet_LoopTimeRange2", [](void*,bool,bool,double* a,double* b,bool) { *a=0; *b=end; });
  FN("CountProjectMarkers", [](void*,int* m,int* r) { *m=1; *r=1; return 2; });
  FN("EnumProjectMarkers3", [](void*,int i,bool* r,double* a,double* b,const char** n,int* id,int* c) {
    *r=i==1; *a=0; *b=10; *n=i ? region_name.c_str() : marker_name.c_str(); *id=i; *c=color; return 1;
  });
  FN("GetLastMarkerAndCurRegion", [](void*,double t,int* m,int* r) { *m=0; *r=t < 10 ? 1 : -1; });
#undef FN
  return nullptr;
}
}
int main() {
  try {
    Host host;
    int reads = 0; uint64_t revision = 0;
    host.current_project = [] { return reinterpret_cast<void*>(1); };
    host.change_count = [](void*) { return changes; };
    host.count_selected_tracks = [&](void*) { ++reads; return int(selection.size()); };
    host.get_selected_track = [](void*,int i) { return reinterpret_cast<void*>(uintptr_t(selection[i])); };
    host.valid_track = [](void*,void*) { return true; };
    host.track_guid = [](void* p) { Guid guid{}; guid[0] = static_cast<unsigned char>(reinterpret_cast<uintptr_t>(p)); return guid; };
    host.event_revision = [&](const std::string&) { return revision; };
    host.native_function = resolve;
    NativeMonitorManager manager; register_native_monitors(manager, host);
    std::map<std::string, Json> events;
    auto tick = [&] { manager.tick(1, NativeMonitorManager::Clock::now()+std::chrono::milliseconds(20), [&](const auto& name,Json data) { events[name]=data; }); };
    tick(); CHECK(reads == 0 && events.empty());
    manager.subscriptions({{"trackSelectionChanged",2}}); tick();
    CHECK(events["trackSelectionChanged"]["count"] == 0 && reads == 1);
    selection={1,2}; ++revision; tick(); auto last=events["trackSelectionChanged"]["revision"];
    CHECK(events["trackSelectionChanged"]["count"] == 2);
    selection={1,3}; ++revision; tick(); CHECK(events["trackSelectionChanged"]["revision"] != last);
    last=events["trackSelectionChanged"]["revision"];
    selection={3,1}; ++revision; tick(); CHECK(events["trackSelectionChanged"]["revision"] != last);
    selection.clear(); ++revision; tick(); CHECK(events["trackSelectionChanged"]["count"] == 0);
    manager.subscriptions({}); const auto previous_reads=reads; ++revision; tick(); CHECK(reads == previous_reads);
    selection.assign(130,1);
    manager.subscriptions({{"trackSelectionChanged",1}}); tick();
    CHECK(manager.snapshot("trackSelectionChanged").is_null());
    manager.subscriptions({}); selection[0]=2;
    manager.subscriptions({{"trackSelectionChanged",2}}); tick(); tick(); tick();
    CHECK(manager.snapshot("trackSelectionChanged")["count"] == 130);
    manager.subscriptions({{"trackStateChanged",1},{"transportChanged",1},{"projectChanged",1},{"markersChanged",1},
      {"regionsChanged",1},{"currentRegionChanged",1},{"loopPointsChanged",1},{"timeSelectionChanged",1}});
    tick(); CHECK(events["trackStateChanged"]["count"] == 3 && events["markersChanged"]["count"] == 1);
    CHECK(events["regionsChanged"]["count"] == 1 && events["currentRegionChanged"]["region"]["name"] == "region");
    const auto before=events;
    state=5; position=11; rate=0.5; dirty=1; extra_project=true; project_path="renamed.rpp"; end=2; color=7;
    marker_name="renamed marker"; region_name="renamed region"; ++changes; ++revision;
    std::this_thread::sleep_for(std::chrono::milliseconds(105)); tick();
    for (const auto& item : events) if (item.first != "trackSelectionChanged") CHECK(item.second != before.at(item.first));
    CHECK(events["transportChanged"]["recording"] == true && events["transportChanged"]["rate"] == 0.5);
    CHECK(events["projectChanged"]["projects"].size() == 2 && events["projectChanged"]["projects"][0]["dirty"] == true);
    CHECK(events["currentRegionChanged"]["region"].is_null());
    CHECK(events["timeSelectionChanged"]["end"] == 2 && events["loopPointsChanged"]["end"] == 2);
    // Same snapshot produces no new revision, including two subscribers.
    const auto stable=events; ++revision; tick(); CHECK(events == stable);
    std::cout << "Native monitor subscription, full selection and state transitions passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
