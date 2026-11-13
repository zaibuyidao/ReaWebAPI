#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

namespace reaweb {
// Two slots are recycled only after the corresponding stream ACK.
struct VideoMemory {
  unsigned char* data = nullptr;
  uint32_t frame_bytes = 0;
  std::string resource_url;
  bool script_copy = false;
  virtual void write_slot(unsigned slot, const void* bytes, uint32_t count = 0) const {
    std::memcpy(data + size_t(slot) * frame_bytes, bytes, count ? count : frame_bytes);
  }
  virtual ~VideoMemory() = default;
};
using VideoMemoryFactory = std::function<std::shared_ptr<VideoMemory>(const std::string&, uint32_t)>;
}
