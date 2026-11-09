#include "video/fingerprint.hpp"
#include "runtime/jsfx_transfer.hpp"
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace reaweb {
bool normalize_video_elf(std::vector<unsigned char>& bytes);
bool normalize_video_macho(std::vector<unsigned char>& bytes);
namespace {
using Bytes = std::vector<unsigned char>;
struct Image {
  Bytes& bytes;
  void range(uint64_t offset, uint64_t size) const {
    if (offset > bytes.size() || size > bytes.size() - offset) throw std::out_of_range("video image");
  }
  uint64_t read(uint64_t offset, unsigned size = 4) const {
    range(offset, size);
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= uint64_t(bytes[size_t(offset) + i]) << (8 * i);
    return value;
  }
  void clear(uint64_t offset, uint64_t size) {
    range(offset, size);
    std::fill_n(bytes.begin() + size_t(offset), size_t(size), 0);
  }
};
void pe(Image image) {
  const auto header = image.read(0x3c), optional = header + 24;
  if (image.read(header) != 0x4550 || image.read(header + 4, 2) != 0x8664 ||
      image.read(optional, 2) != 0x20b) throw std::out_of_range("PE64");
  const auto count = image.read(header + 6, 2), optional_size = image.read(header + 20, 2);
  if (optional_size < 168 || !count || count > 96) throw std::out_of_range("PE sections");
  const auto sections = optional + optional_size;
  image.range(optional, optional_size);
  image.range(sections, count * 40);
  for (uint64_t i = 0; i < count; ++i)
    image.range(image.read(sections + i * 40 + 20), image.read(sections + i * 40 + 16));
  auto file_offset = [&](uint64_t rva, uint64_t size) -> uint64_t {
    for (uint64_t i = 0; i < count; ++i) {
      const auto section = sections + i * 40;
      const auto address = image.read(section + 12), length = image.read(section + 16);
      if (rva >= address && rva - address <= length && size <= length - (rva - address)) {
        const auto offset = image.read(section + 20) + rva - address;
        image.range(offset, size);
        return offset;
      }
    }
    throw std::out_of_range("PE RVA");
  };
  image.clear(header + 8, 4); // COFF timestamp
  image.clear(optional + 64, 4); // PE checksum
  const auto directories = image.read(optional + 108);
  if (directories > (optional_size - 112) / 8) throw std::out_of_range("PE directories");
  if (directories) {
    const auto exports = image.read(optional + 112), size = image.read(optional + 116);
    if (exports && size >= 40) image.clear(file_offset(exports, size) + 4, 4);
  }
  if (directories > 6) {
    const auto debug = image.read(optional + 160), size = image.read(optional + 164);
    if (debug) {
      if (size % 28) throw std::out_of_range("PE debug directory");
      const auto offset = file_offset(debug, size);
      for (uint64_t i = 0; i < size; i += 28) {
        image.clear(offset + i + 4, 4);
        if (image.read(offset + i + 12) == 2) {
          const auto length = image.read(offset + i + 16), raw = image.read(offset + i + 24);
          image.range(raw, length);
          if (length >= 24 && image.read(raw) == 0x53445352) image.clear(raw + 4, 20); // CodeView GUID and age
        }
      }
    }
  }
}
}
std::string video_module_fingerprint(Bytes bytes) {
  try {
    Image image{bytes};
    if (image.read(0, 2) == 0x5a4d) pe(image);
    else if (image.read(0) == 0x464c457f) { if (!normalize_video_elf(bytes)) return {}; }
    else if (!normalize_video_macho(bytes)) return {};
    return JsfxTransfer::sha256(bytes);
  } catch (const std::out_of_range&) { return {}; }
}
}
