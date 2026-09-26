#include "runtime/native_producers.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (0)
thread_local bool realtime = false;
thread_local unsigned allocations = 0;
void* operator new(std::size_t size) { if (realtime) ++allocations; if (auto* p = std::malloc(size ? size : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
using namespace reaweb;
using Clock = std::chrono::steady_clock;
namespace {
double samples[512]{};
int midi_count = 0;
bool device(const char*, char* buffer, int size) { std::snprintf(buffer, size, "%s", "48000"); return true; }
double* audio_buffer(bool, int) { return samples; }
int recent(int index, char* buffer, int* size, int* stamp, int* device, double* position, int* loop) {
  const auto sequence = midi_count - index; if (sequence <= 0) return 0;
  if (*size < 3) return 0;
  buffer[0] = sequence == 1 ? char(0x90) : sequence == 2 ? char(0xb0) : char(0x80);
  buffer[1] = 60; buffer[2] = 100; *size = 3; *stamp = -128; *device = sequence % 2; *position = 1; *loop = 0;
  return sequence;
}
uint64_t published(StreamHub& hub, const std::string& name) {
  const auto state = hub.info();
  for (const auto& stream : state["streams"]) if (stream["name"] == name) return stream["published"].get<uint64_t>();
  return 0;
}
}
int main() {
  try {
    Host host; host.native_function = [](const char* name) -> void* {
      if (!std::strcmp(name, "GetAudioDeviceInfo")) return reinterpret_cast<void*>(device);
      if (!std::strcmp(name, "MIDI_GetRecentInputEvent")) return reinterpret_cast<void*>(recent);
      return nullptr;
    };
    StreamHub streams; NativeProducers producers(host, streams);
    std::vector<std::string> names;
    for (const auto* kind : {"audio", "spectrum", "meter", "waveform"}) {
      names.push_back(producers.audio(kind, Json::object(), 1));
      streams.attach(names.back(), 1, 0, "http://127.0.0.1:1234");
    }
    for (int i = 0; i < 512; ++i) samples[i] = .5 * std::sin(2 * 3.141592653589793 * i / 64);
    realtime = true;
    for (int i = 0; i < 10000; ++i) producers.capture(true, 512, 48000, 0, audio_buffer);
    realtime = false; CHECK(allocations == 0);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
      bool complete = true; for (const auto& name : names) complete = complete && published(streams, name) > 0;
      if (complete) break;
      producers.capture(true, 512, 48000, 0, audio_buffer); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (const auto& name : names) CHECK(published(streams, name) > 0);
    const auto all = producers.midi(-1, 1), selected = producers.midi(1, 1);
    streams.attach(all, 1, 0, "http://127.0.0.1:1234"); streams.attach(selected, 1, 0, "http://127.0.0.1:1234");
    producers.tick(); midi_count = 3; producers.tick();
    CHECK(published(streams, all) == 3 && published(streams, selected) == 2);
    producers.tick(); CHECK(published(streams, all) == 3);
    streams.attach(names.front(), 2, 0, "http://127.0.0.1:1234");
    streams.detach_window(1); producers.close_window(1);
    CHECK(streams.info()["streams"].size() == 1);
    streams.detach_window(2); producers.tick(); CHECK(streams.info()["streams"].empty());
    const auto brief = producers.audio("meter", Json::object(), 3);
    const auto attachment = streams.attach(brief, 3, 0, "http://127.0.0.1:1234");
    producers.attached(brief); streams.detach(attachment["token"], 3);
    producers.tick(); CHECK(streams.info()["streams"].empty());
    std::cout << "PCM capture has zero allocations, worker analysis, MIDI note/CC/filtering and consumer ownership passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
