#include "runtime/video.hpp"
#include "video/pixels.hpp"
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::to_string(__LINE__) + ": " + #x); } while (0)
using namespace reaweb;
namespace {
struct Scene {
  int first = 0, second = 0, changes = 0, play = 0, native_window = 0;
  uint64_t generation = 1;
  void* project = &first;
  double cursor = 1.25, playback = 9.5, speed = 1, fps = 24;
};
Scene* scene;
int play_state() { return scene->play; }
int window_state(int command) { CHECK(command == 50125); return scene->native_window; }
double cursor_position() { return scene->cursor; }
double play_position() { return scene->playback; }
double play_rate(void*) { return scene->speed; }
double frame_rate(void*, bool*) { return scene->fps; }
void* lookup(const char* name) {
  CHECK(std::strncmp(name, "TrackFX_", 8) != 0);
  CHECK(std::strcmp(name, "EnumInstalledFX") != 0);
  CHECK(std::strcmp(name, "Main_OnCommand") != 0);
#define API(n, f) if (!std::strcmp(name, n)) return reinterpret_cast<void*>(f)
  API("GetPlayState", play_state); API("GetToggleCommandState", window_state);
  API("GetCursorPosition", cursor_position); API("GetPlayPosition", play_position);
  API("Master_GetPlayRate", play_rate); API("TimeMap_curFrameRate", frame_rate);
#undef API
  return nullptr;
}
struct BackendState {
  bool supported = true, active = false, malformed = false, throws = false;
  int support_checks = 0, reads = 0, stops = 0;
  VideoRead next = VideoRead::ready;
  std::vector<bool> discontinuities;
};
struct Backend final : VideoBackend {
  std::shared_ptr<BackendState> state;
  explicit Backend(std::shared_ptr<BackendState> s) : state(std::move(s)) {}
  bool supported() override { ++state->support_checks; return state->supported; }
  VideoRead read(bool changed, const Capture& capture) override {
    ++state->reads; state->active = true; state->discontinuities.push_back(changed);
    if (state->throws) throw std::runtime_error("Decoder failure");
    if (state->next == VideoRead::ready) {
      const unsigned char bgra[] = {20, 40, 80, 255, 30, 50, 90, 255};
      capture({bgra, 2, 1, state->malformed ? 1 : 8});
    }
    return state->next;
  }
  void stop() override { ++state->stops; state->active = false; }
};
struct SchedulerState {
  int starts = 0, stops = 0;
  bool throws = false;
  std::function<void()> callback;
  void fire() { const auto current = callback; if (current) current(); }
};
struct Scheduler final : VideoScheduler {
  std::shared_ptr<SchedulerState> state;
  explicit Scheduler(std::shared_ptr<SchedulerState> value) : state(std::move(value)) {}
  void start(std::function<void()> callback) override {
    ++state->starts; state->callback = std::move(callback);
    if (state->throws) throw Error("VIDEO_UNAVAILABLE", "Timer startup failed");
  }
  void stop() override { ++state->stops; state->callback = {}; }
};
struct Fixture {
  Scene state;
  Host host;
  StreamHub streams;
  std::shared_ptr<BackendState> backend = std::make_shared<BackendState>();
  std::shared_ptr<SchedulerState> scheduler = std::make_shared<SchedulerState>();
  std::unique_ptr<VideoManager> video;
  const Json profile{{"width", 4}, {"height", 4}, {"updateRate", 60}};
  explicit Fixture(bool scheduled = false) {
    scene = &state;
    host.native_function = lookup;
    host.current_project = [&] { return state.project; };
    host.project_generation = [&] { return state.generation; };
    host.change_count = [&](void* p) { CHECK(p == state.project); return state.changes; };
    video = std::make_unique<VideoManager>(host, streams, std::make_unique<Backend>(backend),
      scheduled ? std::make_unique<Scheduler>(scheduler) : nullptr);
  }
  Json attach(const std::string& name, int window = 1) {
    return streams.attach(name, window, 0, "http://127.0.0.1:1234");
  }
  std::string open() { auto name = video->open(profile, 1); attach(name); return name; }
  void step() { std::this_thread::sleep_for(std::chrono::milliseconds(25)); video->tick(); }
  uint64_t cleared() { return video->diagnostics()["clearedFrames"].get<uint64_t>(); }
  void ready() {
    backend->next = VideoRead::ready; backend->malformed = backend->throws = false; step();
    CHECK(video->state()["status"] == "ready");
    CHECK(video->state()["sourceWidth"] == 2 && video->state()["sourceHeight"] == 1);
  }
  void invalid(const char* error) {
    const auto value = video->state();
    CHECK(value["status"] == "waiting" && value["error"] == error);
    CHECK(value["sourceWidth"] == 0 && value["sourceHeight"] == 0);
    CHECK(value["position"].is_null() && value["frameAgeMs"].is_null());
  }
};
void missing_native_prerequisites() {
  Host host;
  auto backend = make_video_backend(host);
  CHECK(!backend->supported());
  bool captured = false;
  CHECK(backend->read(false, [&](const VideoImage&) { captured = true; }) == VideoRead::unsupported);
  CHECK(!captured);
  backend->stop();
}
void scheduler_lifetime() {
  Fixture f(true);
  CHECK(f.scheduler->starts == 0 && !f.scheduler->callback);
  const auto name = f.video->open(f.profile, 1);
  CHECK(f.scheduler->starts == 1 && f.scheduler->callback);
  f.scheduler->fire();
  CHECK(f.backend->reads == 0);
  f.attach(name);
  CHECK(f.video->open(f.profile, 2) == name && f.scheduler->starts == 1);
  const auto second = f.attach(name, 2);
  f.scheduler->fire();
  CHECK(f.backend->reads == 1);
  f.video->close_window(1); f.scheduler->fire();
  CHECK(f.scheduler->stops == 0 && f.video->state()["subscribers"] == 1);
  f.streams.detach(second["token"], 2); f.scheduler->fire();
  CHECK(f.scheduler->stops == 1 && !f.scheduler->callback && f.video->state()["status"] == "closed");
  const auto reads = f.backend->reads;
  f.scheduler->fire(); CHECK(f.backend->reads == reads);
  f.open(); CHECK(f.scheduler->starts == 2);
  ++f.state.generation; f.scheduler->fire();
  CHECK(f.scheduler->stops == 2 && !f.scheduler->callback);
  CHECK(f.video->state()["error"] == "VIDEO_PROJECT_CHANGED");
  f.scheduler->throws = true;
  bool rejected = false;
  try { f.video->open(f.profile, 1); } catch (const Error& e) { rejected = e.code == "VIDEO_UNAVAILABLE"; }
  CHECK(rejected && f.scheduler->starts == 3 && f.scheduler->stops == 3 && !f.scheduler->callback);
  CHECK(f.streams.info()["streams"].empty() && f.video->state()["status"] == "closed");
  f.scheduler->throws = false;
  f.open(); CHECK(f.scheduler->starts == 4);
  f.video->shutdown();
  CHECK(f.scheduler->stops == 4 && !f.scheduler->callback);
  f.video->shutdown(); CHECK(f.scheduler->stops == 4);
}
void pixel_conversion() {
  // Padded rows and nonopaque input must produce packed, opaque RGBA.
  const unsigned char bgra[] = {1, 2, 3, 0, 4, 5, 6, 17, 99, 99, 99, 99,
                                7, 8, 9, 48, 10, 11, 12, 255, 99, 99, 99, 99};
  const VideoImage image{bgra, 2, 2, 12};
  std::vector<unsigned char> actual(16, 42);
  copy_video_pixels(image, 2, 2, actual);
  CHECK(actual == std::vector<unsigned char>({3, 2, 1, 255, 6, 5, 4, 255,
                                             9, 8, 7, 255, 12, 11, 10, 255}));
  actual.assign(4 * 2 * 4, 42);
  copy_video_pixels(image, 4, 2, actual);
  CHECK(actual == std::vector<unsigned char>({0, 0, 0, 255, 3, 2, 1, 255, 6, 5, 4, 255, 0, 0, 0, 255,
                                             0, 0, 0, 255, 9, 8, 7, 255, 12, 11, 10, 255, 0, 0, 0, 255}));
  // Compare awkward fractional scales and aspect ratios against direct nearest-neighbor sampling.
  for (int w : {1, 2, 3, 7, 13}) for (int h : {1, 2, 5, 11}) {
    const int stride = w * 4 + 8;
    std::vector<unsigned char> source(size_t(stride) * h);
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<unsigned char>(i * 37);
    for (int width : {1, 2, 4, 9, 16}) for (int height : {1, 3, 8, 17}) {
      actual.assign(size_t(width) * height * 4, 42);
      copy_video_pixels({source.data(), w, h, stride}, width, height, actual);
      const double scale = std::min(double(width) / w, double(height) / h);
      const int dw = std::max(1, int(w * scale)), dh = std::max(1, int(h * scale));
      const int left = (width - dw) / 2, top = (height - dh) / 2;
      for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto offset = (size_t(y) * width + x) * 4;
        CHECK(actual[offset + 3] == 255);
        if (x < left || x >= left + dw || y < top || y >= top + dh) {
          CHECK(actual[offset] == 0 && actual[offset + 1] == 0 && actual[offset + 2] == 0);
        } else {
          const auto at = size_t((y - top) * h / dh) * stride + (x - left) * w / dw * 4;
          CHECK(actual[offset] == source[at + 2] && actual[offset + 1] == source[at + 1] && actual[offset + 2] == source[at]);
        }
      }
    }
  }
}
void unused_and_rejection() {
  Fixture f;
  f.video->tick();
  CHECK(f.video->state()["status"] == "closed");
  CHECK(f.backend->reads == 0 && f.backend->support_checks == 0 && !f.backend->active);
  CHECK(f.streams.info()["streams"].empty());
  const std::vector<Json> invalid = {nullptr, Json::array(), {{"width", 0}}, {{"width", 1921}},
    {{"height", -1}}, {{"height", 1081}}, {{"updateRate", 0}}, {{"updateRate", 75.001}},
    {{"width", 1.5}}, {{"width", true}}, {{"updateRate", "AUTO"}}, {{"updateRate", 0.999}},
    {{"updateRate", std::numeric_limits<double>::quiet_NaN()}}, {{"updateRate", std::numeric_limits<double>::infinity()}},
    {{"updateRate", true}}, {{"updateRate", nullptr}}, {{"source", "file"}}, {{"extra", true}}};
  for (const auto& options : invalid) {
    bool rejected = false;
    try { f.video->open(options, 1); } catch (const Error& e) { rejected = e.code == "INVALID_ARGUMENT"; }
    CHECK(rejected);
  }
  bool window_rejected = false;
  try { f.video->open(f.profile, 0); } catch (const Error& e) { window_rejected = e.code == "INVALID_ARGUMENT"; }
  CHECK(window_rejected && f.backend->reads == 0 && f.backend->support_checks == 0);
  f.backend->supported = false;
  const auto caps = f.video->capabilities();
  CHECK(caps["available"] == false && caps["error"] == "VIDEO_HOST_UNSUPPORTED");
  CHECK(!caps.contains("supportedHosts"));
  CHECK(caps["requiresVideoWindow"] == false && caps["requiresCompanion"] == false);
  bool unsupported = false;
  try { f.video->open(f.profile, 1); } catch (const Error& e) { unsupported = e.code == "VIDEO_HOST_UNSUPPORTED"; }
  CHECK(unsupported && f.backend->reads == 0 && !f.backend->active);
  CHECK(f.streams.info()["streams"].empty());
}
void sharing_and_lifetime() {
  Fixture f;
  CHECK(f.video->capabilities()["available"] == true);
  CHECK(!f.video->capabilities().contains("supportedHosts"));
  const auto name = f.video->open(f.profile, 1);
  CHECK(f.cleared() == 1 && f.video->diagnostics()["capturedFrames"] == 0);
  f.video->tick();
  CHECK(f.backend->reads == 0 && !f.backend->active);
  const auto a = f.attach(name);
  CHECK(f.video->open(f.profile, 2) == name);
  const auto b = f.attach(name, 2);
  f.ready();
  CHECK(f.backend->reads == 1 && f.backend->discontinuities.back());
  CHECK(f.video->state()["subscribers"] == 2 && f.video->state()["position"] == 1.25);
  CHECK(f.video->state()["sourceFrameRate"] == 24);
  CHECK(f.video->diagnostics()["capturedFrames"] == 1);
  CHECK(f.streams.info()["streams"].size() == 1);
  bool conflict = false;
  try { f.video->open(Json{{"width", 8}}, 2); } catch (const Error& e) { conflict = e.code == "VIDEO_PROFILE_CONFLICT"; }
  CHECK(conflict && f.streams.info()["streams"].size() == 1);
  f.state.native_window = 1;
  f.ready();
  CHECK(f.video->state()["videoWindowOpen"] == true);
  f.state.native_window = 0;
  f.ready();
  CHECK(f.video->state()["videoWindowOpen"] == false);
  f.video->close_window(1); f.step();
  CHECK(f.video->state()["subscribers"] == 1 && f.backend->active);
  const auto reads = f.backend->reads;
  f.streams.detach(b["token"], 2); f.video->tick();
  CHECK(f.video->state()["status"] == "closed" && !f.backend->active && f.backend->reads == reads);
  CHECK(f.streams.info()["streams"].empty());
  const auto next = f.open(); CHECK(next != name); f.ready();
  f.video->shutdown();
  CHECK(!f.backend->active && f.video->state()["status"] == "closed");
  const auto stops = f.backend->stops;
  f.video->shutdown(); f.video->tick();
  CHECK(f.backend->stops == stops);
}
void clocks_and_invalidations() {
  Fixture f; f.open(); f.ready();
  auto before = f.cleared();
  f.state.cursor = 7; f.backend->next = VideoRead::pending; f.step();
  f.invalid("VIDEO_FRAME_PENDING");
  CHECK(f.cleared() == before + 1 && f.backend->discontinuities.back());
  f.ready(); CHECK(f.video->state()["position"] == 7);
  before = f.cleared();
  ++f.state.changes; f.backend->next = VideoRead::pending; f.step();
  f.invalid("VIDEO_FRAME_PENDING");
  CHECK(f.cleared() == before + 1 && f.backend->discontinuities.back());
  f.ready();
  for (const auto& failure : std::vector<std::pair<VideoRead, const char*>>{
      {VideoRead::empty, "VIDEO_SOURCE_UNAVAILABLE"},
      {VideoRead::rendering, "VIDEO_RENDER_BUSY"}}) {
    before = f.cleared();
    f.backend->next = failure.first; f.step(); f.invalid(failure.second);
    CHECK(f.cleared() == before + 1);
    f.step(); CHECK(f.cleared() == before + 1);
    f.ready();
  }
  before = f.cleared();
  f.state.play = 1; f.ready(); before = f.cleared();
  const auto captured = f.video->diagnostics()["capturedFrames"];
  f.backend->next = VideoRead::unchanged; f.step();
  CHECK(f.video->state()["status"] == "ready" && f.cleared() == before);
  CHECK(f.video->diagnostics()["capturedFrames"] == captured);
  auto reads = f.backend->reads;
  f.video->tick(); CHECK(f.backend->reads == reads + 1);
  f.backend->next = VideoRead::pending; f.video->tick();
  CHECK(f.video->state()["status"] == "ready" && f.cleared() == before);
  reads = f.backend->reads;
  f.backend->next = VideoRead::ready; f.video->tick();
  CHECK(f.backend->reads == reads + 1 && f.video->diagnostics()["capturedFrames"] == captured.get<int>() + 1);
  f.backend->next = VideoRead::pending;
  std::this_thread::sleep_for(std::chrono::milliseconds(120)); f.video->tick();
  CHECK(f.video->state()["status"] == "ready" && f.cleared() == before);
  std::this_thread::sleep_for(std::chrono::milliseconds(120)); f.video->tick();
  f.invalid("VIDEO_FRAME_PENDING"); CHECK(f.cleared() == before + 1);
  f.ready(); CHECK(f.backend->discontinuities.back());
  f.state.play = 0; f.ready();
  const auto failed = f.video->diagnostics()["captureFailures"].get<uint64_t>();
  f.backend->malformed = true; f.step(); f.invalid("VIDEO_SOURCE_UNAVAILABLE");
  CHECK(f.video->diagnostics()["captureFailures"] == failed + 1);
  f.ready(); f.backend->throws = true; f.step(); f.invalid("VIDEO_SOURCE_UNAVAILABLE");
  CHECK(!f.backend->active && f.video->diagnostics()["captureFailures"] == failed + 2);
  f.ready(); CHECK(f.backend->discontinuities.back());
  f.state.play = 1; f.state.playback = 9.5; f.ready();
  CHECK(f.video->state()["position"] == 9.5 && f.backend->discontinuities.back());
  f.state.play = 2; f.state.cursor = 4; f.ready();
  CHECK(f.video->state()["position"] == 9.5);
  f.state.play = 0; f.ready(); CHECK(f.video->state()["position"] == 4);
}
void source_and_host_changes() {
  Fixture f; f.open(); f.ready();
  const auto epoch = f.video->state()["epoch"].get<uint64_t>();
  f.state.project = &f.state.second; f.video->tick();
  CHECK(f.video->state()["status"] == "closed" && f.video->state()["error"] == "VIDEO_PROJECT_CHANGED");
  CHECK(f.video->state()["epoch"] == epoch + 1 && !f.backend->active);
  f.open(); f.ready(); ++f.state.generation; f.video->tick();
  CHECK(f.video->state()["status"] == "closed" && f.video->state()["error"] == "VIDEO_PROJECT_CHANGED");
  CHECK(!f.backend->active && f.streams.info()["streams"].empty());
  f.open(); f.ready(); f.backend->next = VideoRead::unsupported; f.step();
  CHECK(f.video->state()["status"] == "closed" && f.video->state()["error"] == "VIDEO_HOST_UNSUPPORTED");
  CHECK(!f.backend->active && f.streams.info()["streams"].empty());
}
void automatic_rate() {
  Fixture f;
  const Json options{{"width", 4}, {"height", 4}, {"updateRate", "auto"}};
  CHECK(f.video->capabilities()["autoUpdateRate"] == true);
  CHECK(f.video->capabilities()["defaultUpdateRate"] == "auto");
  CHECK(f.video->capabilities()["maxUpdateRate"] == 75);
  f.state.fps = 24000.0 / 1001;
  const auto name = f.video->open({{"width", 4}, {"height", 4}}, 1);
  const auto ticket = f.attach(name);
  CHECK(ticket["updateRate"] == f.state.fps);
  f.video->tick();
  CHECK(f.video->state()["updateRate"] == "auto");
  CHECK(f.video->state()["targetUpdateRate"] == f.state.fps);
  for (double fps : {23.976, 24.0, 25.0, 29.97, 30000.0 / 1001, 30.0, 48.0, 50.0, 60.0, 75.0, 2.0, 120.0, 0.5, 0.0,
                     std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    f.state.fps = fps;
    f.video->tick();
    const double expected = !std::isfinite(fps) || fps <= 0 ? 30 : std::clamp(fps, 1.0, 75.0);
    CHECK(f.video->state()["targetUpdateRate"] == expected);
    CHECK(f.video->open(options, 2) == name);
    CHECK(f.video->state()["stream"] == name);
  }
  f.state.fps = 2; f.video->tick();
  const int before = f.backend->reads;
  f.step(); CHECK(f.backend->reads == before);
  f.state.fps = 50; f.video->tick(); CHECK(f.backend->reads == before + 1);
  bool conflict = false;
  try { f.video->open({{"width", 4}, {"height", 4}, {"updateRate", 50}}, 2); }
  catch (const Error& error) { conflict = error.code == "VIDEO_PROFILE_CONFLICT"; }
  CHECK(conflict);
  f.streams.detach(ticket["token"], 1); f.video->tick();
  f.open(); f.state.fps = 24; f.ready();
  CHECK(f.video->state()["updateRate"] == 60 && f.video->state()["targetUpdateRate"] == 60);
  f.video->shutdown();
}
void fixed_fractional_rates() {
  for (double rate : {1.0, 23.976, 24000.0 / 1001, 24.0, 25.0, 29.97, 30000.0 / 1001,
                      30.0, 48.0, 50.0, 60.0, 75.0}) {
    Fixture f;
    const Json options{{"width", 4}, {"height", 4}, {"updateRate", rate}};
    const auto name = f.video->open(options, 1);
    const auto ticket = f.attach(name);
    CHECK(ticket["updateRate"] == rate);
    f.video->tick();
    CHECK(f.video->state()["updateRate"] == rate && f.video->state()["targetUpdateRate"] == rate);
    f.state.fps = 12; f.video->tick();
    CHECK(f.video->state()["targetUpdateRate"] == rate);
    CHECK(f.video->open(options, 2) == name);
    bool conflict = false;
    try { f.video->open({{"width", 4}, {"height", 4}, {"updateRate", rate + 0.001}}, 2); }
    catch (const Error& error) { conflict = error.code == (rate == 75 ? "INVALID_ARGUMENT" : "VIDEO_PROFILE_CONFLICT"); }
    CHECK(conflict);
  }
}
void automatic_frame_clock() {
  Fixture f;
  f.state.play = 1; f.state.fps = 60;
  f.attach(f.video->open({{"width", 4}, {"height", 4}, {"updateRate", "auto"}}, 1));
  f.video->tick();
  const auto captured = f.video->diagnostics()["capturedFrames"].get<uint64_t>();
  const auto reads = f.backend->reads;
  f.backend->next = VideoRead::unchanged; f.video->tick();
  CHECK(f.backend->reads == reads + 1 && f.video->diagnostics()["capturedFrames"] == captured);
  f.backend->next = VideoRead::ready; f.state.playback += 1.0 / 60; f.video->tick();
  CHECK(f.video->diagnostics()["capturedFrames"] == captured + 1);
  f.state.speed = 2; const auto throttled = f.backend->reads; f.video->tick();
  CHECK(f.backend->reads == throttled);
}
}
int main() {
  try {
    pixel_conversion(); scheduler_lifetime(); missing_native_prerequisites(); unused_and_rejection(); sharing_and_lifetime(); clocks_and_invalidations(); source_and_host_changes(); automatic_rate(); fixed_fractional_rates(); automatic_frame_clock();
    std::cout << "Video host rejection, consumer ownership, invalidation, clocks and cleanup passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
