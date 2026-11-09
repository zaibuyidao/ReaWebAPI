#include "video/fingerprint.hpp"
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace reaweb {
namespace {
using Bytes = std::vector<unsigned char>;
struct MachImage {
  Bytes& bytes;
  void range(uint64_t offset, uint64_t size) const {
    if (offset > bytes.size() || size > bytes.size() - offset) throw std::out_of_range("Mach-O image");
  }
  uint64_t read(uint64_t offset, unsigned size = 4, bool big = false) const {
    range(offset, size);
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
      value |= uint64_t(bytes[size_t(offset) + i]) << (8 * (big ? size - i - 1 : i));
    return value;
  }
  void clear(uint64_t offset, uint64_t size) {
    range(offset, size);
    std::fill_n(bytes.begin() + size_t(offset), size_t(size), 0);
  }
};
}
bool normalize_video_macho(std::vector<unsigned char>& bytes) {
  try {
    MachImage image{bytes};
    const auto magic = image.read(0, 4, true);
    if (magic == 0xcafebabe || magic == 0xcafebabf) {
      const auto count = image.read(4, 4, true);
      const bool wide = magic == 0xcafebabf;
      const uint64_t entry_size = wide ? 32 : 20, table_end = 8 + count * entry_size;
      if (!count || count > 64) return false;
      image.range(8, count * entry_size);
      uint64_t selected = 0, length = 0;
      for (uint64_t i = 0; i < count; ++i) {
        const auto entry = 8 + i * entry_size;
        const auto offset = image.read(entry + 8, wide ? 8 : 4, true);
        const auto size = image.read(entry + (wide ? 16 : 12), wide ? 8 : 4, true);
        image.range(offset, size);
        if (offset < table_end || size < 32) return false;
        if (image.read(entry, 4, true) == 0x100000c) {
          if (selected) return false;
          selected = offset; length = size;
        }
      }
      if (!selected) return false;
      Bytes slice(bytes.begin() + size_t(selected), bytes.begin() + size_t(selected + length));
      bytes.swap(slice);
    }
    if (image.read(0) != 0xfeedfacf || image.read(4) != 0x100000c || image.read(12) != 6) return false;
    const auto count = image.read(16), command_size = image.read(20), end = 32 + command_size;
    if (!count || count > command_size / 8) return false;
    image.range(32, command_size);
    uint64_t cursor = 32, uuid = 0, signature = 0, signature_size = 0, linkedit = 0, linkedit_size = 0;
    for (uint64_t i = 0; i < count; ++i) {
      if (cursor > end || end - cursor < 8) return false;
      const auto command = image.read(cursor), size = image.read(cursor + 4);
      if (size < 8 || size % 8 || size > end - cursor) return false;
      if (command == 0x1b) {
        if (size != 24 || uuid) return false;
        uuid = cursor + 8;
      } else if (command == 0x1d) {
        if (size != 16 || signature) return false;
        signature = image.read(cursor + 8); signature_size = image.read(cursor + 12);
        if (!signature || !signature_size) return false;
        image.range(signature, signature_size);
      } else if (command == 0x19) {
        if (size < 72) return false;
        const auto offset = image.read(cursor + 40, 8), length = image.read(cursor + 48, 8);
        image.range(offset, length);
        const auto sections = image.read(cursor + 64);
        if (sections > (size - 72) / 80 || size != 72 + sections * 80) return false;
        const unsigned char name[] = {'_', '_', 'L', 'I', 'N', 'K', 'E', 'D', 'I', 'T', 0, 0, 0, 0, 0, 0};
        if (std::equal(std::begin(name), std::end(name), bytes.begin() + size_t(cursor + 8))) {
          if (linkedit || sections) return false;
          linkedit = offset; linkedit_size = length;
        }
      }
      cursor += size;
    }
    if (cursor != end) return false;
    if (signature) {
      // A code signature must be the trailing payload of __LINKEDIT, outside load commands.
      if (signature < end || !linkedit || signature < linkedit ||
          signature - linkedit > linkedit_size || signature_size != linkedit_size - (signature - linkedit) ||
          signature + signature_size != bytes.size()) return false;
      image.clear(signature, signature_size);
    }
    if (uuid) image.clear(uuid, 16);
    return true;
  } catch (const std::out_of_range&) {
    return false;
  }
}
}
