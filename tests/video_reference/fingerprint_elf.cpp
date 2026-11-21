#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reaweb {
bool normalize_video_elf(std::vector<unsigned char>& bytes) {
  const auto range = [&](uint64_t offset, uint64_t size) {
    return offset <= bytes.size() && size <= bytes.size() - offset;
  };
  const auto read = [&](size_t offset, unsigned size) {
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= uint64_t(bytes[offset + i]) << (i * 8);
    return value;
  };
  if (!range(0, 64) || std::memcmp(bytes.data(), "\x7f" "ELF", 4) ||
      bytes[4] != 2 || bytes[5] != 1 || bytes[6] != 1 || read(20, 4) != 1 || read(52, 2) != 64)
    return false;
  const auto phoff = read(32, 8), shoff = read(40, 8);
  const auto phsize = read(54, 2), phnum = read(56, 2), shsize = read(58, 2), shnum = read(60, 2);
  if (phnum == 0xffff || (phnum && (phsize != 56 || !range(phoff, phnum * phsize))) ||
      !shnum || shsize != 64 || !range(shoff, shnum * shsize)) return false;
  for (uint64_t i = 0; i < phnum; ++i) {
    const auto entry = phoff + i * phsize;
    if (!range(read(entry + 8, 8), read(entry + 32, 8))) return false;
  }
  bool found = false;
  for (uint64_t i = 0; i < shnum; ++i) {
    const auto entry = shoff + i * shsize, type = read(entry + 4, 4);
    const auto offset = read(entry + 24, 8), size = read(entry + 32, 8);
    if (type == 8) continue; // SHT_NOBITS has no file payload.
    if (!range(offset, size)) return false;
    if (type != 7) continue; // SHT_NOTE
    uint64_t cursor = offset;
    const auto end = offset + size;
    while (cursor < end) {
      if (end - cursor < 12) return false;
      const auto namesize = read(cursor, 4), descsize = read(cursor + 4, 4), kind = read(cursor + 8, 4);
      cursor += 12;
      const auto namepad = (namesize + 3) & ~uint64_t(3), descpad = (descsize + 3) & ~uint64_t(3);
      if (namepad > end - cursor || descpad > end - cursor - namepad) return false;
      if (kind == 3 && namesize == 4 && !std::memcmp(bytes.data() + cursor, "GNU\0", 4)) {
        if (!descsize) return false;
        std::fill(bytes.begin() + cursor + namepad, bytes.begin() + cursor + namepad + descsize, 0);
        found = true;
      }
      cursor += namepad + descpad;
    }
  }
  return found;
}
}
