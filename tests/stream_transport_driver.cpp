#include "runtime/native_stream.hpp"
#include <iostream>
#include <thread>
using namespace reaweb;
int main() {
  StreamHub hub; ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = 1;
  desc.format = REAWEB_PIXEL_RGBA8; desc.width = 240; desc.height = 160; desc.stride = 960;
  desc.max_bytes = desc.stride * desc.height; desc.capacity = 3; desc.update_rate = 60;
  uint64_t handle = 0; if (hub.create(REAWEB_FRAME, "test.frame", &desc, &handle)) return 1;
  std::cout << Json::array({hub.attach("test.frame", 1, 0, "http://127.0.0.1:9000"), hub.attach("test.frame", 2, 0, "http://127.0.0.1:9000")}).dump() << std::endl;
  std::vector<unsigned char> pixels(desc.max_bytes);
  auto begin = std::chrono::steady_clock::now();
  for (unsigned sequence = 1; sequence <= 120; ++sequence) {
    std::fill(pixels.begin(), pixels.end(), static_cast<unsigned char>(sequence));
    if (hub.publish(REAWEB_FRAME, handle, pixels.data(), static_cast<uint32_t>(pixels.size()), sequence, sequence / 60.0)) return 2;
    std::this_thread::sleep_until(begin + std::chrono::microseconds(sequence * 16667));
  }
  hub.close(handle); std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::cout << hub.info().dump() << std::endl;
}
