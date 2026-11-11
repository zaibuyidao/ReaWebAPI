#include "runtime/native_stream.hpp"
#include <iostream>
#include <thread>
using namespace reaweb;
int main(int argc,char** argv) {
  const bool timing_packets=argc>1 && std::string(argv[1])=="timing";
  const bool video=argc>1 && std::string(argv[1]).rfind("video",0)==0;
  const bool shared_video=argc>1 && std::string(argv[1])=="video-shared";
  const bool pressure=argc>1 && std::string(argv[1])=="video-pressure";
  const bool jitter=argc>2 && std::string(argv[2])=="jitter";
  const bool burst=argc>2 && std::string(argv[2])=="burst";
  const char* name = video ? "reaweb.video.project.test" : "test.frame";
  StreamHub hub; ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = 1;
  desc.format = REAWEB_PIXEL_RGBA8; desc.width = 240; desc.height = 160; desc.stride = 960;
  if(video) { desc.width=1280;desc.height=720;desc.stride=5120; }
  if(argc>1 && std::string(argv[1])=="video-4k") { desc.width=3840;desc.height=2160;desc.stride=15360; }
  if(argc>1 && std::string(argv[1])=="video-small") { desc.width=4;desc.height=4;desc.stride=16; }
  if(argc>1 && std::string(argv[1])=="video-medium") { desc.width=64;desc.height=64;desc.stride=256; }
  if(argc>1 && std::string(argv[1])=="frame-max") { desc.width=2048;desc.height=2048;desc.stride=8192; }
  desc.max_bytes = desc.stride * desc.height; desc.capacity = 3; desc.update_rate = 60;
  if(pressure) { desc.width=3840;desc.height=2160;desc.stride=15360;desc.max_bytes=desc.stride*desc.height; }
  uint64_t handle = 0;
  const auto created=timing_packets?hub.create_jsfx(REAWEB_FRAME,name,&desc,&handle,1,0,{{"sessionId","test"}}):hub.create(REAWEB_FRAME,name,&desc,&handle,video);
  if(created)return 1;
  struct Memory : VideoMemory {
    std::vector<unsigned char> storage;
    explicit Memory(uint32_t bytes) : storage(size_t(bytes) * 2) { data = storage.data(); frame_bytes = bytes; }
  };
  const VideoMemoryFactory memory = shared_video || pressure ? VideoMemoryFactory([](const std::string&, uint32_t bytes) { return std::make_shared<Memory>(bytes); }) : VideoMemoryFactory{};
  auto tickets = Json::array({hub.attach(name, 1, 0, "http://127.0.0.1:9000", memory), hub.attach(name, timing_packets?1:2, 0, "http://127.0.0.1:9000")});
  if(pressure) for(int window=3;window<=8;++window) tickets.push_back(hub.attach(name,window,0,"http://127.0.0.1:9000"));
  std::cout << tickets.dump() << std::endl;
  std::vector<unsigned char> pixels(desc.max_bytes);
  auto begin = std::chrono::steady_clock::now();
  auto publish = [&](unsigned sequence) {
    std::fill(pixels.begin(), pixels.end(), static_cast<unsigned char>(sequence));
    const StreamBuffer::Timing timing{99,9007199254740992ULL+sequence,17,0.5,1,true};
    const auto status = hub.publish(REAWEB_FRAME, handle, pixels.data(), static_cast<uint32_t>(pixels.size()), sequence, sequence / 60.0,timing_packets?&timing:nullptr,video && sequence % 3 == 1);
    // A descheduled producer can catch up while transport owns a buffer slot.
    if (status != REAWEB_OK && status != REAWEB_BUFFER_FULL) {
      std::cerr << "Frame publication failed: " << status << std::endl;
      return false;
    }
    return true;
  };
  if (jitter || burst || pressure) {
    if (!publish(1)) return 2;
    std::string command;
    while (std::getline(std::cin, command) && command != "close") {
      const auto first = static_cast<unsigned>(std::stoul(command));
      for (unsigned i = 0; i < (burst ? 3u : 1u); ++i) if (!publish(first + i)) return 2;
      std::cout << "published" << std::endl;
    }
  } else for (unsigned sequence = 1; sequence <= 120; ++sequence) {
    if (!publish(sequence)) return 2;
    std::this_thread::sleep_until(begin + std::chrono::microseconds(sequence * 16667));
  }
  const double latency = hub.frame_ack_ms(handle);
  hub.close(handle);
  // Keep transport alive until the consumer has received the complete close.
  std::string done;
  if (!std::getline(std::cin, done) || done != "done") return 3;
  auto diagnostics = hub.info(); diagnostics["frameLatencyMs"] = latency;
  std::cout << diagnostics.dump() << std::endl;
}
