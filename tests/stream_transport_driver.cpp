#include "runtime/native_stream.hpp"
#include <iostream>
#include <thread>
using namespace reaweb;
int main(int argc,char** argv) {
  const bool timing_packets=argc>1 && std::string(argv[1])=="timing";
  const bool video=argc>1 && std::string(argv[1])=="video";
  StreamHub hub; ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = 1;
  desc.format = REAWEB_PIXEL_RGBA8; desc.width = 240; desc.height = 160; desc.stride = 960;
  if(video) { desc.width=1280;desc.height=720;desc.stride=5120; }
  desc.max_bytes = desc.stride * desc.height; desc.capacity = 3; desc.update_rate = 60;
  uint64_t handle = 0;
  const auto created=timing_packets?hub.create_jsfx(REAWEB_FRAME,"test.frame",&desc,&handle,1,0,{{"sessionId","test"}}):hub.create(REAWEB_FRAME,"test.frame",&desc,&handle);
  if(created)return 1;
  std::cout << Json::array({hub.attach("test.frame", 1, 0, "http://127.0.0.1:9000"), hub.attach("test.frame", timing_packets?1:2, 0, "http://127.0.0.1:9000")}).dump() << std::endl;
  std::vector<unsigned char> pixels(desc.max_bytes);
  auto begin = std::chrono::steady_clock::now();
  for (unsigned sequence = 1; sequence <= 120; ++sequence) {
    std::fill(pixels.begin(), pixels.end(), static_cast<unsigned char>(sequence));
    const StreamBuffer::Timing timing{99,9007199254740992ULL+sequence,17,0.5,1,true};
    const auto status = hub.publish(REAWEB_FRAME, handle, pixels.data(), static_cast<uint32_t>(pixels.size()), sequence, sequence / 60.0,timing_packets?&timing:nullptr);
    // A descheduled producer can catch up while transport owns a buffer slot.
    if (status != REAWEB_OK && status != REAWEB_BUFFER_FULL) {
      std::cerr << "Frame publication failed: " << status << std::endl;
      return 2;
    }
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
