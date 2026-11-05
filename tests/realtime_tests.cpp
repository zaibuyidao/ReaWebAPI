#include "runtime/realtime.hpp"
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
}
int main() {
  try {
    main_thread = std::this_thread::get_id();
    Host host; host.current_project = [] { CHECK(std::this_thread::get_id() == main_thread); return static_cast<void*>(&project); };
    host.project_generation = [] { return generation; };
    host.native_function = [](const char* name) -> void* {
      if (!std::strcmp(name, "GetPlayPosition")) return reinterpret_cast<void*>(+[] {
        ++reads; return position;
      });
      if (!std::strcmp(name, "GetPlayState")) return reinterpret_cast<void*>(+[] { return play_state; });
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
