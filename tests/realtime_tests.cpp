#include "runtime/realtime.hpp"
#include "runtime/track_controls.hpp"
#include "runtime/track_controls.hpp"
#include <cstring>
#include <iostream>
using namespace reaweb;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("Check failed: " #x); } while (false)
namespace {
std::atomic<int> reads{0};
int play_state = 1, project = 0;
uint64_t generation = 1;
double position = 3;
std::thread::id main_thread;
struct Scheduler : RealtimeScheduler {
  std::function<void()> callback;
  double rate = 0;
  bool supported() const override { return true; }
  void start(double hz, std::function<void()> cb) override { rate = hz; callback = std::move(cb); }
  void stop() override { rate = 0; callback = {}; }
  void fire() { if (callback) callback(); }
};
void fails(const std::function<void()>& fn, const char* code) {
  try { fn(); } catch (const Error& error) { CHECK(error.code == code); return; }
  throw std::runtime_error("Expected failure");
}
void check_tracks() {
  const std::string a = "{11111111-1111-1111-1111-111111111111}";
  const std::string b = "{22222222-2222-2222-2222-222222222222}";
  std::vector<std::string> order{a,b};
  static std::array<double,4> values{1.0,0.0,0.0,2.0};
  static unsigned queries = 0;
  uint64_t revision = 0, list = 0, gen = 0;
  Host host;
  host.current_project = [] { return static_cast<void*>(&project); };
  host.project_generation = [&] { return gen; };
  host.track_count = [&] { return static_cast<int>(order.size()); };
  host.track_identity = [&](int i) { return order.at(i); };
  host.event_revision = [&](const std::string& name) { return name == "track-list" ? list : revision; };
  host.native_function = [](const char* name) -> void* {
    if (!std::strcmp(name,"GetTrack")) return reinterpret_cast<void*>(+[](ReaProject*, int) { return reinterpret_cast<MediaTrack*>(&values); });
    if (!std::strcmp(name,"GetMediaTrackInfo_Value")) return reinterpret_cast<void*>(+[](MediaTrack*, const char* key) {
      CHECK(std::this_thread::get_id() == main_thread); ++queries;
      const char* keys[] = {"D_VOL","D_PAN","B_MUTE","I_SOLO"};
      for (int i = 0; i < 4; ++i) if (!std::strcmp(key,keys[i])) return values[i];
      throw std::runtime_error("Unknown property");
    });
    return nullptr;
  };
  TrackControls controls(host);
  for (const Json& ids : {Json::array(), Json::array({1}), Json::array({"bad"}), Json::array({a,a}),
                         Json(std::vector<std::string>(17,a))})
    fails([&] { controls.watch({{"tracks",ids}}); }, "INVALID_ARGUMENT");
  for (const Json& props : {Json::array(),Json::array({"width"}),Json::array({"pan","pan"})})
    fails([&] { controls.watch({{"tracks",{a}},{"properties",props}}); }, "INVALID_ARGUMENT");
  StreamHub streams;
  auto clock = std::make_unique<Scheduler>();
  auto* scheduler = clock.get();
  RealtimeManager manager(host,streams,std::move(clock));
  Json options{{"tracks",{b,a}},{"properties",{"volume","solo"}}};
  auto descriptor = manager.subscribe("track-controls",options,1);
  CHECK(scheduler->rate == 0 && descriptor["mask"] == 9 && descriptor["tracks"] == options["tracks"]);
  manager.housekeeping();
  CHECK(manager.diagnostics(1)["subscriptions"][0]["published"] == 1);
  auto concurrent = manager.subscribe("track-controls",options,2);
  manager.close_window(1);
  CHECK(manager.diagnostics(2)["subscriptions"].size() == 1);
  manager.housekeeping();
  CHECK(manager.diagnostics(2)["subscriptions"][0]["published"] == 1);
  manager.unsubscribe(concurrent["id"],2);
  const auto idle = queries;
  manager.housekeeping(); CHECK(queries == idle);
  queries = 0;
  CHECK(manager.diagnostics(1)["subscriptions"].empty() && streams.info()["streams"].empty());
  auto first = controls.watch(options), second = controls.watch(options);
  uint64_t handle, seq = 0, published = 0, dropped = 0;
  ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = REAWEB_STREAM_ABI;
  desc.format = REAWEB_BYTES; desc.capacity = 4; desc.max_bytes = TrackControls::maxBytes; desc.source = "test";
  CHECK(streams.create(REAWEB_BINARY,"tracks.test",&desc,&handle) == REAWEB_OK);
  auto sample = [&](TrackControls::Watch& w) { return controls.sample(w,120,streams,handle,seq,published,dropped); };
  controls.begin();
  CHECK(!sample(first) && !sample(second) && queries == 4 && published == 2);
  std::swap(order[0],order[1]); ++list; ++revision;
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  controls.begin();
  CHECK(!sample(first) && first.indices[0] == 0 && published == 2 && first.unchanged == 1);
  values[3] = 5; ++revision;
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  controls.begin(); CHECK(!sample(first) && published == 3 && first.previous[1] == 5);
  order.clear(); ++list;
  controls.begin(); CHECK(sample(first) == stream_track_gone);
  fails([&] { controls.watch(options); }, "STALE_HANDLE");
  ++gen;
  controls.begin(); CHECK(sample(second) == stream_project_changed);
}

void check_fx_parameters() {
  const std::string track = "{11111111-1111-1111-1111-111111111111}";
  const std::string fx = "{22222222-2222-2222-2222-222222222222}";
  const std::string other = "{33333333-3333-3333-3333-333333333333}";
  static std::vector<std::string> chain;
  chain = {fx,other};
  static unsigned reads = 0;
  static int count = 33;
  static double value = 0.25;
  uint64_t revision = 0, epoch = 1;
  Host host;
  host.current_project = [] { return static_cast<void*>(&project); };
  host.project_generation = [&] { return epoch; };
  host.track_count = [] { return 1; };
  host.track_identity = [&](int i) { CHECK(i == 0); return track; };
  host.fx_identity = [](void*, int i, bool) { return chain.at(i); };
  host.event_revision = [&](const std::string&) { return revision; };
  host.native_function = [](const char* key) -> void* {
    if (!std::strcmp(key,"GetTrack")) return reinterpret_cast<void*>(+[](ReaProject*,int) { return reinterpret_cast<MediaTrack*>(&project); });
    if (!std::strcmp(key,"TrackFX_GetCount")) return reinterpret_cast<void*>(+[](MediaTrack*) { return int(chain.size()); });
    if (!std::strcmp(key,"TrackFX_GetNumParams")) return reinterpret_cast<void*>(+[](MediaTrack*,int) { return count; });
    if (!std::strcmp(key,"TrackFX_GetParamNormalized")) return reinterpret_cast<void*>(+[](MediaTrack*,int i,int index) {
      CHECK(main_thread == std::this_thread::get_id()); ++reads;
      return chain[i][1] == '2' ? (index == 32 ? value : 0.5) : 0.0;
    });
    return nullptr;
  };
  StreamHub streams;
  RealtimeManager manager(host,streams,std::make_unique<Scheduler>());
  Json options{{"track",track},{"fx",fx},{"parameters",{32,0}}};
  CHECK(manager.capabilities()["sources"]["fx-parameters"]["available"] == true);
  for (const Json& bad : {Json::array(),Json::array({1,1}),Json::array({-1}),Json::array({33}),
                          Json::array({0.5}),Json::array({4294967296ULL}),Json(std::vector<int>(33,0))}) {
    auto invalid = options; invalid["parameters"] = bad;
    fails([&] { manager.subscribe("fx-parameters",invalid,1); }, "INVALID_ARGUMENT");
  }
  for (const auto* field : {"track","fx"}) {
    auto invalid = options; invalid[field] = 0;
    fails([&] { manager.subscribe("fx-parameters",invalid,1); }, "INVALID_ARGUMENT");
  }
  auto missing = options; missing["track"] = other;
  fails([&] { manager.subscribe("fx-parameters",missing,1); }, "STALE_HANDLE");
  auto first = manager.subscribe("fx-parameters",options,1);
  CHECK(first["parameters"] == options["parameters"] && first["schema"] == "fx-parameters.v1");
  manager.subscribe("fx-parameters",options,2);
  manager.housekeeping(); CHECK(reads == 2);
  auto row = [&] { return manager.diagnostics(2)["subscriptions"].at(0); };
  CHECK(row()["published"] == 1);
  auto tick = [&] { std::this_thread::sleep_for(std::chrono::milliseconds(40)); manager.housekeeping(); };
  std::swap(chain[0],chain[1]); ++revision; tick();
  CHECK(row()["unchanged"] == 1 && reads == 4);
  value = 0.8; tick(); CHECK(row()["published"] == 2 && reads == 6);
  manager.close_window(1); CHECK(manager.diagnostics(1)["subscriptions"].empty());
  count = 1; tick(); CHECK(manager.diagnostics(2)["subscriptions"].empty());
  count = 33;
  manager.subscribe("fx-parameters",options,2);
  chain[1] = other; tick(); CHECK(manager.diagnostics(2)["subscriptions"].empty());
  chain[1] = fx; manager.subscribe("fx-parameters",options,2);
  ++epoch; manager.housekeeping(); CHECK(manager.diagnostics(2)["subscriptions"].empty());
  const auto idle = reads; tick(); CHECK(reads == idle);
}
void check_fx_chains() {
  static int regular, master, take;
  const std::string tr = "{11111111-1111-1111-1111-111111111111}";
  const std::string mt = "{22222222-2222-2222-2222-222222222222}";
  const std::string tk = "{33333333-3333-3333-3333-333333333333}";
  const std::string fx = "{44444444-4444-4444-4444-444444444444}";
  const int input = 0x1000000, child = 0x2000005;
  static bool validTake = true;
  static unsigned calls = 0;
  static std::map<std::pair<void*,int>,std::string> ids;
  ids.clear();
  Host h;
  h.current_project = [] { return static_cast<void*>(&project); };
  h.track_identity = [&](int) { return tr; };
  h.track_count = [] { return 1; };
  h.master_by_guid = [&](void*,const std::string& id) -> void* { return id == mt ? &master : nullptr; };
  h.take_by_guid = [&](void*,void* track,const std::string& id) -> void* {
    return validTake && track == &regular && id == tk ? &take : nullptr;
  };
  h.fx_identity = [](void* owner,int index,bool isTake) {
    CHECK((owner == &take) == isTake);
    const auto it = ids.find({owner,index});
    return it == ids.end() ? std::string() : it->second;
  };
  h.native_function = [](const char* name) -> void* {
    const std::string key(name);
    if (key == "GetTrack") return reinterpret_cast<void*>(+[](ReaProject*,int) { return reinterpret_cast<MediaTrack*>(&regular); });
    if (key == "TrackFX_GetCount" || key == "TrackFX_GetRecCount" || key == "TakeFX_GetCount")
      return reinterpret_cast<void*>(+[](void*) { return 1; });
    if (key == "TrackFX_GetNamedConfigParm" || key == "TakeFX_GetNamedConfigParm")
      return reinterpret_cast<void*>(+[](void*,int i,const char* key,char* out,int size) {
        if (i & 0x2000000) return false;
        if (!std::strcmp(key,"container_count")) std::snprintf(out,size,"1");
        else if (!std::strcmp(key,"container_item.0")) std::snprintf(out,size,"%d",i | 0x2000005);
        else return false;
        return true;
      });
    if (key == "TrackFX_SetParamNormalized" || key == "TakeFX_SetParamNormalized")
      return reinterpret_cast<void*>(+[](void* owner,int index,int param,double value) {
        CHECK(main_thread == std::this_thread::get_id());
        CHECK(ids.count({owner,index}) == 1 && param == 1);
        return value == 0.5;
      });
    if (key == "TrackFX_GetNumParams" || key == "TakeFX_GetNumParams")
      return reinterpret_cast<void*>(+[](void*,int) { return 2; });
    if (key == "TrackFX_GetParamNormalized" || key == "TakeFX_GetParamNormalized")
      return reinterpret_cast<void*>(+[](void* owner,int index,int param) {
        CHECK(main_thread == std::this_thread::get_id());
        CHECK(ids.count({owner,index}) == 1 && param == 1); ++calls; return 0.625;
      });
    return nullptr;
  };
  StreamHub streams;
  RealtimeManager m(h,streams,std::make_unique<Scheduler>());
  TrackControls writer(h,TrackControls::Source::fxParameters);
  CHECK(m.capabilities()["sources"]["fx-parameters"]["chains"].size() == 4);
  for (const auto& scope : {"main","input","monitoring","take","master-main"}) {
    const bool masterScope = !std::strcmp(scope,"monitoring") || !std::strcmp(scope,"master-main");
    const bool tkScope = !std::strcmp(scope,"take");
    const bool rec = !std::strcmp(scope,"input") || !std::strcmp(scope,"monitoring");
    const std::string chain = !std::strcmp(scope,"master-main") ? "main" : scope;
    Json opt{{"track",masterScope ? mt : tr},{"fx",fx},{"chain",chain},{"parameters",{1}}};
    if (tkScope) opt["take"] = tk;
    for (const int offset : {0,child}) {
      void* owner = tkScope ? &take : masterScope ? &master : &regular;
      int address = offset | (rec ? input : 0);
      ids = {{{owner,address},fx}};
      auto ref = writer.getFxRef(opt,7);
      CHECK(writer.setFxParameter(ref,1,0.5,7));
      fails([&]{writer.setFxParameter(ref,1,0.5,8);},"PROJECT_CHANGED");
      fails([&]{writer.setFxParameter(ref,2,0.5,7);},"INVALID_ARGUMENT");
      fails([&]{writer.setFxParameter(ref,1,1.1,7);},"INVALID_ARGUMENT");
      fails([&]{writer.setFxParameter(ref,1,0.6,7);},"NATIVE_ERROR");
      auto wrong = opt;
      if (tkScope) wrong["take"] = mt;
      else wrong["chain"] = rec ? "main" : masterScope ? "monitoring" : "input";
      fails([&]{m.subscribe("fx-parameters",wrong,1);},"STALE_HANDLE");
      m.subscribe("fx-parameters",opt,1); m.subscribe("fx-parameters",opt,2);
      auto before = calls;
      m.housekeeping(); CHECK(calls == before + 1);
      ids.clear(); validTake = !tkScope;
      fails([&]{writer.setFxParameter(ref,1,0.5,7);},"STALE_HANDLE");
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      m.housekeeping(); CHECK(m.diagnostics(1)["subscriptions"].empty() && m.diagnostics(2)["subscriptions"].empty());
      validTake = true;
    }
  }
}

}
int main() {
  try {
    main_thread = std::this_thread::get_id();
    check_tracks();
    check_fx_parameters();
    check_fx_chains();
    Host host; host.current_project = [] { CHECK(std::this_thread::get_id() == main_thread); return static_cast<void*>(&project); };
    host.project_generation = [] { return generation; };
    host.native_function = [](const char* name) -> void* {
      if (!std::strcmp(name, "GetPlayPosition")) return reinterpret_cast<void*>(+[] {
        ++reads; return position;
      });
      if (!std::strcmp(name, "GetPlayState")) return reinterpret_cast<void*>(+[] { return play_state; });
      if (!std::strcmp(name, "Master_GetPlayRateAtTime")) return reinterpret_cast<void*>(+[](double t, void*) { CHECK(t == position); return 1.25; });
      if (!std::strcmp(name, "TimeMap_GetTimeSigAtTime")) return reinterpret_cast<void*>(+[](void*, double t, int* n, int* d, double* bpm) {
        CHECK(t == position); *n = t < 2 ? 4 : 7; *d = t < 2 ? 4 : 8; *bpm = 90 + t * 15;
      });
      if (!std::strcmp(name, "TimeMap_timeToQN_abs")) return reinterpret_cast<void*>(+[](void*, double t) { CHECK(t == position); return t * 1.5 + t * t / 8; });
      throw std::runtime_error("Unexpected realtime getter");
    };
    StreamHub streams; auto clock = std::make_unique<Scheduler>(); auto* scheduler = clock.get();
    RealtimeManager realtime(host, streams, std::move(clock));
    CHECK(realtime.capabilities()["sources"]["transport"]["available"] == true);
    CHECK(scheduler->rate == 0);
    const auto diagnostics = realtime.diagnostics(1);
    CHECK(diagnostics.size() == 4 && diagnostics.contains("schedulerRate") && diagnostics.contains("skippedBusy"));
    CHECK(diagnostics["sources"]["transport"].size() == 4);
    CHECK(diagnostics["sources"]["transport"].contains("sampled") && diagnostics["sources"]["transport"].contains("errors"));
    CHECK(diagnostics["sources"]["transport"].contains("maxSampleIntervalMs") && diagnostics["sources"]["transport"].contains("lastSampleAt"));
    fails([&] { realtime.subscribe("missing", Json::object(), 1); }, "REALTIME_SOURCE_NOT_FOUND");
    for (auto options : {Json{{"updateRate", 0}}, Json{{"updateRate", 121}}, Json{{"updateRate", "120"}}, Json{{"extra", 1}}})
      fails([&] { realtime.subscribe("transport", options, 1); }, "INVALID_ARGUMENT");
    const auto a = realtime.subscribe("transport", {{"updateRate", 60}}, 1);
    const auto b = realtime.subscribe("transport", {{"updateRate", 120}}, 2);
    CHECK(a["stream"] != b["stream"] && scheduler->rate == 120);
    const auto descriptor = streams.attach(a["stream"], 1, 1, "reaweb://test");
    CHECK(!descriptor.contains("layoutVersion") && !descriptor.contains("jsfx"));
    scheduler->fire(); CHECK(reads == 1);
    CHECK(realtime.diagnostics(1)["subscriptions"][0]["published"] == 1);
    CHECK(realtime.diagnostics(2)["subscriptions"][0]["published"] == 1);
    scheduler->fire(); CHECK(reads == 1);
    fails([&] { realtime.unsubscribe(a["id"], 2); }, "INVALID_ARGUMENT");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    generation++; position = 0; play_state = 0; realtime.housekeeping();
    std::thread([&] { scheduler->fire(); }).join(); CHECK(reads == 2);
    CHECK(realtime.diagnostics(1)["sources"]["transport"]["errors"] == 0);
    // All musical getters must receive the single position read, including
    // reverse jumps at a loop boundary and paused/recording states.
    for (const auto state : {1, 2, 5, 6, 0}) {
      for (const auto time : {0.125, 1.999, 2.0, 12.625, 0.25}) {
        play_state = state; position = time;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::thread([&] { scheduler->fire(); }).join();
      }
    }
    CHECK(realtime.diagnostics(1)["sources"]["transport"]["errors"] == 0);
    realtime.close_window(2); CHECK(scheduler->rate == 60);
    CHECK(realtime.diagnostics(2)["subscriptions"].empty());
    realtime.housekeeping();
    streams.detach_window(1); realtime.housekeeping();
    CHECK(scheduler->rate == 0 && streams.info()["streams"].empty());
    realtime.unsubscribe(a["id"], 1);
    auto c = realtime.subscribe("transport", Json::object(), 1);
    CHECK(c["id"] != a["id"]);
    realtime.shutdown(); CHECK(scheduler->rate == 0 && streams.info()["streams"].empty());
    fails([&] { realtime.subscribe("transport", Json::object(), 1); }, "EXTENSION_UNLOADED");
    RealtimeManager native(host, streams);
    native.subscribe("transport", {{"updateRate", 120}}, 1);
    const auto before = reads.load();
    // No main-thread pumping: sampling must continue independently. Wait for
    // progress rather than assuming a shared CI runner can deliver 10 samples
    // within 300 ms; this is a lifecycle test, not a scheduling benchmark.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (reads.load() - before < 10 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (reads.load() - before < 10)
      std::cerr << "Native sampling timed out: " << (reads.load() - before)
                << " reads; diagnostics: " << native.diagnostics(1).dump() << '\n';
    CHECK(reads.load() - before >= 10);
    for (int i = 0; i < 20; ++i) {
      auto sub = native.subscribe("transport", {{"updateRate", 60}}, 2);
      native.unsubscribe(sub["id"], 2);
    }
    native.shutdown(); const auto stopped = reads.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(30)); CHECK(reads == stopped);
    CHECK(streams.info()["streams"].empty());
    std::cout << "Realtime lifecycle, shared sampling, native scheduling and compatibility passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
