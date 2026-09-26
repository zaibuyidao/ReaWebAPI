#include "runtime/native_stream.hpp"
#include <cstring>
#include <iostream>
#include <thread>
#include <stdexcept>
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (0)
using namespace reaweb;
int main() {
  try {
    ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = 1;
    desc.format = REAWEB_PIXEL_RGBA8; desc.max_bytes = 64; desc.capacity = 3; desc.width = 4; desc.height = 4; desc.stride = 16;
    StreamBuffer latest(REAWEB_FRAME, desc); unsigned char pixels[64]{};
    for (unsigned i = 0; i < 10000; ++i) { std::memset(pixels, i & 255, sizeof(pixels)); CHECK(latest.publish(pixels, sizeof(pixels), i, i * 0.01) == 0); }
    StreamBuffer::Packet packet; CHECK(latest.consume(packet)); CHECK(packet.sequence == 9999); CHECK(packet.data.front() == (9999 & 255));
    CHECK(latest.dropped > 0); CHECK(!latest.consume(packet));
    CHECK(latest.publish(pixels, 63, 1, 0) == REAWEB_INVALID_ARGUMENT);
    std::atomic<bool> done{false}; std::thread producer([&] {
      for (unsigned i = 1; i <= 100000; ++i) { std::memset(pixels, i & 255, sizeof(pixels)); latest.publish(pixels, sizeof(pixels), i, 0); }
      done = true;
    });
    uint64_t last = 0; bool valid = true;
    for (;;) {
      if (!latest.consume(packet)) { if (done) break; continue; }
      if (packet.sequence <= last) { std::cerr << "Sequence regression " << last << " -> " << packet.sequence << '\n'; valid = false; }
      last = packet.sequence;
      for (auto byte : packet.data) if (byte != (packet.sequence & 255)) { std::cerr << "Torn frame " << packet.sequence << '\n'; valid = false; break; }
    }
    producer.join(); CHECK(valid && last);
    desc.format = REAWEB_FLOAT32; desc.channels = 2; desc.sample_rate = 48000; desc.block_frames = 8;
    StreamBuffer audio(REAWEB_AUDIO, desc); float pcm[16]{};
    for (int i = 0; i < 3; ++i) CHECK(audio.publish(pcm, sizeof(pcm), i, 0) == 0);
    CHECK(audio.publish(pcm, sizeof(pcm), 3, 0) == REAWEB_BUFFER_FULL);
    for (int i = 0; i < 3; ++i) { CHECK(audio.consume(packet)); CHECK(packet.sequence == uint64_t(i)); }
    CHECK(!audio.consume(packet)); CHECK(audio.publish(pcm, sizeof(pcm), 4, 0) == 0);
    CHECK(audio.consume(packet)); CHECK(packet.sequence == 4 && packet.dropped == 1);
    StreamHub hub; uint64_t handle = 0;
    CHECK(hub.create(REAWEB_AUDIO, "test.audio", &desc, &handle) == 0);
    uint64_t duplicate = 0; CHECK(hub.create(REAWEB_AUDIO, "test.audio", &desc, &duplicate) == REAWEB_SERVICE_EXISTS);
    CHECK(hub.publish(REAWEB_FRAME, handle, pcm, sizeof(pcm), 1, 0) == REAWEB_UNSUPPORTED_FORMAT);
    CHECK(hub.publish(REAWEB_AUDIO, handle, pcm, sizeof(pcm), 1, 0) == 0);
    auto a = hub.attach("test.audio", 1, 0, "http://127.0.0.1:1234");
    auto b = hub.attach("test.audio", 2, 0, "http://127.0.0.1:1234");
    hub.detach(a["token"], 2); CHECK(hub.info()["consumers"] == 2);
    hub.detach_window(1); CHECK(hub.info()["consumers"] == 1);
    CHECK(hub.publish(REAWEB_AUDIO, handle, pcm, sizeof(pcm), 2, 0) == 0);
    CHECK(hub.close(handle) == 0); CHECK(hub.close(handle) == 0);
    CHECK(hub.publish(REAWEB_AUDIO, handle, pcm, sizeof(pcm), 3, 0) == REAWEB_STREAM_CLOSED);
    CHECK(hub.create(REAWEB_AUDIO, "test.audio", &desc, &duplicate) == 0 && duplicate != handle);
    CHECK(hub.close(handle) == REAWEB_STREAM_CLOSED); CHECK(hub.info()["streams"].size() == 1);
    std::cout << "Native stream buffers, concurrency, policies and lifetime passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
