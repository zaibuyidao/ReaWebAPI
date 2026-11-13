#include "runtime/native_stream.hpp"
#include <condition_variable>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace reaweb;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (0)

struct Source final : VideoPresentationSource {
  struct Frame { uint64_t sequence; double timestamp; bool clear; std::vector<unsigned char> bytes; VideoFrameLayout layout; };
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Frame> frames;
  bool fail = false;
  void submit(const VideoPresentationFrame& frame) override {
    if (fail) throw std::runtime_error("GPU allocation failure");
    std::lock_guard<std::mutex> lock(mutex);
    frames.push_back({frame.sequence, frame.timestamp, frame.discontinuity,
                      {frame.data, frame.data + frame.bytes}, {frame.width, frame.height, frame.stride}});
    changed.notify_all();
  }
  void wait(size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(changed.wait_for(lock, std::chrono::seconds(5), [&] { return frames.size() >= count; }));
  }
  std::vector<Frame> snapshot() {
    std::lock_guard<std::mutex> lock(mutex); return frames;
  }
};
struct Presenter final : VideoPresenter {
  std::shared_ptr<Source> input;
  std::atomic<bool> invalid{false};
  std::mutex mutex;
  std::condition_variable changed;
  explicit Presenter(std::shared_ptr<Source> source) : input(std::move(source)) {}
  std::shared_ptr<VideoPresentationSource> source() const override { return input; }
  bool closed() const override { return invalid; }
  void layout(const Json& value) override { validate_video_surface_layout(value); }
  Json diagnostics() const override {
    return {{"renderer", "test"}, {"closed", closed()}, {"error", input->fail ? "GPU allocation failure" : ""}};
  }
  void invalidate() noexcept override {
    std::lock_guard<std::mutex> lock(mutex); invalid = true; changed.notify_all();
  }
  void wait_closed() {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(changed.wait_for(lock, std::chrono::seconds(5), [&] { return invalid.load(); }));
  }
};
template<class F> bool rejected(F&& call, const char* code = nullptr) {
  try { call(); } catch (const Error& error) { return !code || error.code == code; }
  return false;
}
int main() {
  try {
    Json rect = {{"x", -100}, {"y", 0}, {"width", 3840}, {"height", 2160}};
    Json layout = {{"rect", rect}, {"clip", rect}};
    validate_video_surface_layout(layout);
    layout["clip"]["width"] = 0; validate_video_surface_layout(layout);
    layout["preserveAspect"] = false; validate_video_surface_layout(layout);
    auto bad = layout; bad["rect"]["width"] = -1;
    CHECK(rejected([&] { validate_video_surface_layout(bad); }, "INVALID_ARGUMENT"));
    bad = layout; bad["rect"]["x"] = std::numeric_limits<double>::infinity();
    CHECK(rejected([&] { validate_video_surface_layout(bad); }, "INVALID_ARGUMENT"));
    bad = layout; bad["clip"]["extra"] = 1;
    CHECK(rejected([&] { validate_video_surface_layout(bad); }, "INVALID_ARGUMENT"));
    bad = layout; bad["preserveAspect"] = 1;
    CHECK(rejected([&] { validate_video_surface_layout(bad); }, "INVALID_ARGUMENT"));
    CHECK(rejected([&] { validate_video_surface_layout(rect); }, "INVALID_ARGUMENT"));

    StreamHub hub;
    ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = 1;
    desc.format = REAWEB_PIXEL_RGBA8; desc.max_bytes = 64; desc.capacity = 3;
    desc.width = 4; desc.height = 4; desc.stride = 16;
    uint64_t handle = 0;
    CHECK(hub.create(REAWEB_FRAME, "surface.video", &desc, &handle, true) == REAWEB_OK);
    auto source = std::make_shared<Source>();
    std::vector<std::shared_ptr<Presenter>> views;
    std::vector<std::string> tokens;
    for (int window = 1; window <= 5; ++window) {
      auto view = std::make_shared<Presenter>(source); views.push_back(view);
      auto info = hub.attach_video_presenter("surface.video", window, 7, "reaweb://test", [&](const Json& value) {
        CHECK(!value.contains("url") && value["width"] == 4); return view;
      });
      CHECK(!info.contains("url") && info["renderer"] == "test"); tokens.push_back(info["token"]);
    }
    CHECK(hub.consumers(handle) == 5);
    CHECK(hub.video_presenter(tokens[0], 1, 7) == views[0]);
    CHECK(!hub.video_presenter(tokens[0], 2, 7) && !hub.video_presenter(tokens[0], 1, 8));
    unsigned char pixels[64]; std::fill(std::begin(pixels), std::end(pixels), 37);
    CHECK(hub.publish(REAWEB_FRAME, handle, pixels, sizeof(pixels), 11, 0.25) == REAWEB_OK);
    source->wait(1);
    auto frames = source->snapshot();
    CHECK(frames.size() == 1 && frames[0].sequence == 11 && frames[0].timestamp == 0.25);
    CHECK(!frames[0].clear && frames[0].bytes == std::vector<unsigned char>(64, 37));

    // A distinct late presenter receives the retained frame while playback is paused.
    auto late_source = std::make_shared<Source>(); auto late_view = std::make_shared<Presenter>(late_source);
    hub.attach_video_presenter("surface.video", 6, 7, "reaweb://test", [&](const Json&) { return late_view; });
    late_source->wait(1); CHECK(late_source->snapshot()[0].sequence == 11);
    CHECK(source->snapshot().size() == 1);
    hub.detach(tokens[0], 2); CHECK(!views[0]->closed() && hub.consumers(handle) == 6);
    hub.detach(tokens[0], 1); CHECK(views[0]->closed() && !views[1]->closed());
    CHECK(!hub.video_presenter(tokens[0], 1, 7));
    CHECK(hub.publish(REAWEB_FRAME, handle, pixels, sizeof(pixels), 12, 0.5, nullptr, true) == REAWEB_OK);
    source->wait(2); late_source->wait(2);
    frames = source->snapshot(); CHECK(frames.size() == 2 && frames.back().sequence == 12 && frames.back().clear);

    // A native renderer failure must not terminate delivery to the other source.
    auto broken = std::make_shared<Source>(); broken->fail = true;
    auto broken_view = std::make_shared<Presenter>(broken);
    const auto broken_info = hub.attach_video_presenter("surface.video", 7, 7, "reaweb://test", [&](const Json&) { return broken_view; });
    broken_view->wait_closed();
    CHECK(hub.publish(REAWEB_FRAME, handle, pixels, sizeof(pixels), 13, 0.75) == REAWEB_OK);
    source->wait(3); late_source->wait(3);
    CHECK(source->snapshot().back().sequence == 13);
    CHECK(hub.consumers(handle) == 5);
    const auto failure = hub.video_presenter_diagnostics(broken_info["token"], 7, 7);
    CHECK(failure["closed"] == true && failure["errorCode"] == "VIDEO_RENDERER_UNAVAILABLE");
    CHECK(failure["error"] == "GPU allocation failure");
    CHECK(hub.video_presenter_diagnostics(broken_info["token"], 6, 7) == Json({{"closed", true}}));
    CHECK(hub.video_presenter_diagnostics(broken_info["token"], 7, 8) == Json({{"closed", true}}));
    // Native failures are reclaimed even when JavaScript never polls diagnostics.
    views[3]->invalidate();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (hub.consumers(handle) != 4 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(hub.consumers(handle) == 4 && !hub.video_presenter(tokens[3], 4, 7));
    hub.detach_window(2); CHECK(views[1]->closed());
    hub.detach_stream_window(handle, 3); CHECK(views[2]->closed());
    CHECK(hub.close(handle) == REAWEB_OK);
    for (const auto& view : views) CHECK(view->closed());
    CHECK(late_view->closed() && hub.consumers(handle) == 0);

    {
      uint64_t dynamic = 0;
      CHECK(hub.create(REAWEB_FRAME, "surface.dynamic", &desc, &dynamic, true, {{"dynamicResolution", true}}) == REAWEB_OK);
      auto input = std::make_shared<Source>(); auto view = std::make_shared<Presenter>(input);
      const auto ticket = hub.attach_video_presenter("surface.dynamic", 9, 0, "reaweb://test", [&](const Json& info) {
        CHECK(info["dynamicResolution"] == true && info["layoutVersion"] == 3); return view;
      });
      unsigned sequence = 0;
      for (const auto layout : {VideoFrameLayout{8, 4, 32}, {4, 8, 16}, {2, 2, 8}}) {
        std::vector<unsigned char> bytes(size_t(layout.stride) * layout.height, 42);
        CHECK(hub.publish(REAWEB_FRAME, dynamic, bytes.data(), uint32_t(bytes.size()), ++sequence, 0, nullptr, false, nullptr, &layout) == REAWEB_OK);
        input->wait(sequence); const auto delivered = input->snapshot().back();
        CHECK(delivered.layout.width == layout.width && delivered.layout.height == layout.height && delivered.layout.stride == layout.stride);
        CHECK(delivered.bytes == bytes && !view->closed());
      }
      auto replay = std::make_shared<Source>(); auto replay_view = std::make_shared<Presenter>(replay);
      hub.attach_video_presenter("surface.dynamic", 10, 0, "reaweb://test", [&](const Json& info) {
        CHECK(info["width"] == 2 && info["height"] == 2); return replay_view;
      });
      replay->wait(1); const auto retained = replay->snapshot().front();
      CHECK(retained.sequence == sequence && retained.layout.width == 2 && retained.layout.height == 2 && retained.bytes.size() == 16);
      CHECK(hub.video_presenter(ticket["token"], 9, 0) == view && hub.consumers(dynamic) == 2);
      CHECK(hub.close(dynamic) == REAWEB_OK);
    }

    uint64_t ordinary = 0;
    CHECK(hub.create(REAWEB_FRAME, "surface.ordinary", &desc, &ordinary) == REAWEB_OK);
    bool called = false;
    CHECK(rejected([&] { hub.attach_video_presenter("surface.ordinary", 8, 0, "reaweb://test", [&](const Json&) {
      called = true; return late_view;
    }); }));
    CHECK(!called && hub.consumers(ordinary) == 0);
    uint64_t retry = 0;
    CHECK(hub.create(REAWEB_FRAME, "surface.retry", &desc, &retry, true) == REAWEB_OK);
    CHECK(rejected([&] { hub.attach_video_presenter("surface.retry", 8, 0, "reaweb://test", [&](const Json&) -> std::shared_ptr<VideoPresenter> {
      throw Error("VIDEO_PRESENTATION_UNAVAILABLE", "test");
    }); }, "VIDEO_PRESENTATION_UNAVAILABLE"));
    CHECK(hub.consumers(retry) == 0);
    CHECK(rejected([&] { hub.attach_video_presenter("surface.retry", 8, 0, "reaweb://test", [&](const Json&) -> std::shared_ptr<VideoPresenter> { return {}; }); }));
    CHECK(hub.consumers(retry) == 0);
    std::cout << "Video surface sharing and lifecycle passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
