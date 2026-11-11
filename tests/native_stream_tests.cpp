#include "runtime/native_stream.hpp"
#include <cstring>
#include <iostream>
#include <thread>
#include <stdexcept>
#ifdef __linux__
#include "platform/linux/video_memory.hpp"
#endif
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (0)
using namespace reaweb;
int main() {
  try {
#ifdef __linux__
    {
      auto memory = std::make_unique<LinuxVideoMemory>(64);
      CHECK(memory->data && memory->fd >= 0);
      LinuxVideoResource resource{getpid(), memory->fd, uint64_t(memory->identity.st_dev), uint64_t(memory->identity.st_ino), 64};
      const int fd = resource.open(); CHECK(fd >= 0);
      unsigned char bytes[64]; std::memset(bytes, 37, sizeof(bytes)); memory->write_slot(1, bytes);
      auto mapping = static_cast<unsigned char*>(mmap(nullptr, 128, PROT_READ, MAP_SHARED, fd, 0));
      CHECK(mapping != MAP_FAILED && mapping[64] == 37);
      CHECK(pwrite(fd, bytes, sizeof(bytes), 0) == -1);
      CHECK(ftruncate(memory->fd, 64) == -1);
      ++resource.inode; CHECK(resource.open() == -1);
      memory.reset(); CHECK(mapping[64] == 37);
      munmap(mapping, 128); ::close(fd);
      CHECK(!LinuxVideoMemory(0).data && !LinuxVideoMemory(41u * 1024 * 1024).data);
    }
#endif
    ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = 1;
    desc.format = REAWEB_PIXEL_RGBA8; desc.max_bytes = 64; desc.capacity = 3; desc.width = 4; desc.height = 4; desc.stride = 16;
    StreamBuffer latest(REAWEB_FRAME, desc); unsigned char pixels[64]{};
    for (unsigned i = 0; i < 10000; ++i) { std::memset(pixels, i & 255, sizeof(pixels)); CHECK(latest.publish(pixels, sizeof(pixels), i, i * 0.01) == 0); }
    StreamBuffer::Packet packet; CHECK(latest.consume(packet)); CHECK(packet.sequence == 9999); CHECK(packet.data.front() == (9999 & 255));
    CHECK(latest.dropped > 0); CHECK(!latest.consume(packet));
    StreamBuffer video(REAWEB_FRAME, desc, true);
    for (unsigned i = 1; i <= 3; ++i) CHECK(video.publish(pixels, sizeof(pixels), i, i / 60.0, nullptr, i == 2) == 0);
    for (unsigned i = 1; i <= 3; ++i) { CHECK(video.consume(packet)); CHECK(packet.sequence == i); CHECK(packet.discontinuity == (i == 2)); }
    CHECK(!video.consume(packet));
    for (unsigned i = 4; i <= 8; ++i) CHECK(video.publish(pixels, sizeof(pixels), i, i / 60.0, nullptr, i == 2) == 0);
    for (unsigned i = 6; i <= 8; ++i) { CHECK(video.consume(packet)); CHECK(packet.sequence == i); CHECK(packet.discontinuity == (i == 2)); }
    CHECK(!video.consume(packet) && video.dropped == 2);
    {
      StreamBuffer owned(REAWEB_FRAME, desc, true);
      std::vector<unsigned char> frame(64, 17);
      const auto* original = frame.data();
      CHECK(owned.publish(frame.data(), 63, 1, 0, nullptr, false, &frame) == REAWEB_INVALID_ARGUMENT);
      CHECK(frame.data() == original && frame.front() == 17);
      CHECK(latest.publish(frame.data(), 64, 1, 0, nullptr, false, &frame) == REAWEB_INVALID_ARGUMENT);
      CHECK(frame.data() == original);
      CHECK(owned.publish(frame.data(), 64, 1, 0, nullptr, false, &frame) == REAWEB_OK);
      CHECK(frame.data() != original && frame.size() == 64);
      std::fill(frame.begin(), frame.end(), 42);
      CHECK(owned.consume(packet) && packet.data.data() == original);
      CHECK(std::all_of(packet.data.begin(), packet.data.end(), [](auto byte) { return byte == 17; }));
      CHECK(owned.publish(frame.data(), 64, 2, 0, nullptr, false, &frame) == REAWEB_OK);
      CHECK(std::all_of(packet.data.begin(), packet.data.end(), [](auto byte) { return byte == 17; }));
      CHECK(owned.consume(packet) && packet.sequence == 2);
      CHECK(std::all_of(packet.data.begin(), packet.data.end(), [](auto byte) { return byte == 42; }));
    }
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
    done = false; valid = true; last = 8;
    producer = std::thread([&] {
      std::vector<unsigned char> frame(64);
      for (unsigned i = 9; i <= 100000; ++i) {
        std::fill(frame.begin(), frame.end(), i & 255);
        video.publish(frame.data(), uint32_t(frame.size()), i, 0, nullptr, false, &frame);
      }
      done = true;
    });
    for (;;) {
      if (!video.consume(packet)) { if (done) break; continue; }
      if (packet.sequence <= last) valid = false;
      last = packet.sequence;
      for (auto byte : packet.data) if (byte != (packet.sequence & 255)) valid = false;
    }
    producer.join(); CHECK(valid && last > 8);
    {
      auto fifo_desc = desc; fifo_desc.capacity = 128;
      StreamBuffer fifo(REAWEB_FRAME, fifo_desc, true);
      constexpr uint64_t total = 200000;
      std::atomic<uint64_t> acknowledged{0};
      std::atomic<bool> finished{false};
      bool published_ok = true;
      std::thread writer([&] {
        unsigned char frame[64]{};
        for (uint64_t first = 1; first <= total; first += 2) {
          while (first > acknowledged.load() + 1) std::this_thread::yield();
          for (uint64_t sequence = first; sequence < first + 2; ++sequence) {
            frame[0] = static_cast<unsigned char>(sequence);
            if (fifo.publish(frame, sizeof(frame), sequence, 0) != REAWEB_OK) published_ok = false;
          }
        }
        finished = true;
      });
      uint64_t received = 0, previous = 0;
      bool ordered = true;
      for (;;) {
        if (!fifo.consume(packet)) { if (finished && acknowledged == total) break; continue; }
        ordered &= packet.sequence == previous + 1 && packet.data.front() == (packet.sequence & 255);
        previous = packet.sequence; ++received; acknowledged = previous;
      }
      writer.join();
      CHECK(published_ok && fifo.dropped == 0);
      CHECK(ordered && received == total);
    }
    desc.format = REAWEB_FLOAT32; desc.channels = 2; desc.sample_rate = 48000; desc.block_frames = 8;
    StreamBuffer audio(REAWEB_AUDIO, desc); float pcm[16]{};
    for (int i = 0; i < 3; ++i) CHECK(audio.publish(pcm, sizeof(pcm), i, 0) == 0);
    CHECK(audio.publish(pcm, sizeof(pcm), 3, 0) == REAWEB_BUFFER_FULL);
    for (int i = 0; i < 3; ++i) { CHECK(audio.consume(packet)); CHECK(packet.sequence == uint64_t(i)); }
    CHECK(!audio.consume(packet)); CHECK(audio.publish(pcm, sizeof(pcm), 4, 0) == 0);
    CHECK(audio.consume(packet)); CHECK(packet.sequence == 4 && packet.dropped == 1);
    const StreamBuffer::Timing timing{7,9007199254740993ULL,11,0.25,1,true};
    CHECK(audio.publish(pcm,sizeof(pcm),5,0,&timing)==0);
    CHECK(audio.consume(packet)&&packet.timing&&packet.timing->first_sample==9007199254740993ULL);
    CHECK(packet.timing->clock_epoch==7&&packet.timing->source_dropped==11&&packet.timing->discontinuity);
    CHECK(audio.publish(pcm,sizeof(pcm),6,0)==0);
    CHECK(audio.consume(packet)&&!packet.timing);
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
    uint64_t protected_handle=0;
    CHECK(hub.create_jsfx(REAWEB_AUDIO,"test.jsfx",&desc,&protected_handle,7,3,{{"sessionId","opaque"},{"id","output"}})==0);
    const auto rejected=[&](int window,uint64_t generation,bool external) {
      try { if(external)hub.attach_external("test.jsfx",42);else hub.attach("test.jsfx",window,generation,"reaweb://same"); }
      catch(const Error& e){return e.code=="FX_UNBOUND";}
      return false;
    };
    CHECK(rejected(8,3,false)&&rejected(7,2,false)&&rejected(0,0,true));
    const auto own=hub.attach("test.jsfx",7,3,"reaweb://same");
    CHECK(own["layoutVersion"]==2&&own["jsfx"]["sessionId"]=="opaque");
    hub.detach_window(7);CHECK(hub.consumers(protected_handle)==0);CHECK(hub.close(protected_handle)==0);
    {
      struct Memory : VideoMemory {
        std::vector<unsigned char> storage;
        explicit Memory(uint32_t bytes) : storage(size_t(bytes) * 2) { data = storage.data(); frame_bytes = bytes; }
      };
      std::weak_ptr<VideoMemory> memory;
      int allocations = 0;
      const VideoMemoryFactory factory = [&](const std::string& token, uint32_t bytes) {
        CHECK(token.size() == 64); ++allocations;
        auto result = std::make_shared<Memory>(bytes); memory = result; return result;
      };
      const auto ordinary = hub.attach("test.audio", 20, 0, "reaweb://test", factory);
      CHECK(allocations == 0 && !ordinary.contains("sharedVideo"));
      hub.detach_window(20);
      desc.format = REAWEB_PIXEL_RGBA8;
      uint64_t video_handle = 0;
      CHECK(hub.create(REAWEB_FRAME, "test.video.memory", &desc, &video_handle, true) == REAWEB_OK);
      const auto mapped = hub.attach("test.video.memory", 21, 0, "reaweb://test", factory);
      CHECK(mapped["sharedVideo"] == true && allocations == 1 && !memory.expired());
      hub.detach(mapped["token"], 22); CHECK(!memory.expired());
      hub.detach(mapped["token"], 21); CHECK(memory.expired());
      const auto fallback = hub.attach("test.video.memory", 21, 0, "reaweb://test",
        [](const std::string&, uint32_t) -> std::shared_ptr<VideoMemory> { throw std::bad_alloc(); });
      CHECK(!fallback.contains("sharedVideo"));
      hub.detach_window(21); CHECK(hub.close(video_handle) == REAWEB_OK);
      // No frame is published: only mapping quota and delayed release are exercised.
      struct BudgetMemory : VideoMemory {
        unsigned char unused = 0;
        explicit BudgetMemory(uint32_t bytes) { data = &unused; frame_bytes = bytes; }
      };
      desc.width = 8192; desc.height = 1280; desc.stride = desc.width * 4; desc.max_bytes = video_max_frame_bytes;
      CHECK(hub.create(REAWEB_FRAME, "test.video.budget", &desc, &video_handle, true) == REAWEB_OK);
      std::vector<std::shared_ptr<VideoMemory>> retained;
      const VideoMemoryFactory budget_factory = [&](const std::string&, uint32_t bytes) {
        auto result = std::make_shared<BudgetMemory>(bytes); retained.push_back(result); return result;
      };
      for (int window = 30; window < 33; ++window)
        CHECK(hub.attach("test.video.budget", window, 0, "reaweb://test", budget_factory)["sharedVideo"] == true);
      CHECK(!hub.attach("test.video.budget", 33, 0, "reaweb://test", budget_factory).contains("sharedVideo"));
      hub.detach_window(30);
      CHECK(!hub.attach("test.video.budget", 34, 0, "reaweb://test", budget_factory).contains("sharedVideo"));
      retained.erase(retained.begin());
      CHECK(hub.attach("test.video.budget", 35, 0, "reaweb://test", budget_factory)["sharedVideo"] == true);
      for (int window = 30; window <= 35; ++window) hub.detach_window(window);
      retained.clear(); CHECK(hub.close(video_handle) == REAWEB_OK);
    }
    std::cout << "Native stream buffers, concurrency, policies and lifetime passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
