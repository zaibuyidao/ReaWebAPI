#include "video/fingerprint.hpp"
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace reaweb { bool normalize_video_macho(std::vector<unsigned char>&); }
void video_fingerprint_macho_tests() {
  using Bytes = std::vector<unsigned char>;
  auto put = [](Bytes& b, size_t at, uint64_t n, unsigned size = 4, bool big = false) {
    for (unsigned i = 0; i < size; ++i) b.at(at + i) = static_cast<unsigned char>(n >> (8 * (big ? size - i - 1 : i)));
  };
  auto check = [](bool ok) { if (!ok) throw std::runtime_error("Mach-O fingerprint regression"); };
  Bytes original(512, 0);
  put(original, 0, 0xfeedfacf); put(original, 4, 0x100000c); put(original, 12, 6);
  put(original, 16, 3); put(original, 20, 112);
  put(original, 32, 0x19); put(original, 36, 72);
  const char name[] = "__LINKEDIT";
  std::copy(name, name + sizeof(name), original.begin() + 40);
  put(original, 72, 256, 8); put(original, 80, 256, 8);
  put(original, 104, 0x1b); put(original, 108, 24);
  put(original, 128, 0x1d); put(original, 132, 16); put(original, 136, 384); put(original, 140, 128);
  auto canonical = original;
  check(reaweb::normalize_video_macho(canonical));
  auto changed = original;
  std::fill(changed.begin() + 112, changed.begin() + 128, 42);
  std::fill(changed.begin() + 384, changed.end(), 23);
  check(reaweb::normalize_video_macho(changed) && changed == canonical);
  changed = original; changed[200] = 1;
  check(reaweb::normalize_video_macho(changed) && changed != canonical);
  changed = original; put(changed, 136, 120);
  check(!reaweb::normalize_video_macho(changed));
  changed = original; put(changed, 36, 0);
  check(!reaweb::normalize_video_macho(changed));
  changed = original; changed.resize(100);
  check(!reaweb::normalize_video_macho(changed));
  changed = original; put(changed, 4, 0x1000007);
  check(!reaweb::normalize_video_macho(changed));
  Bytes universal(32 + original.size(), 0);
  put(universal, 0, 0xcafebabe, 4, true); put(universal, 4, 1, 4, true);
  put(universal, 8, 0x100000c, 4, true); put(universal, 16, 32, 4, true);
  put(universal, 20, original.size(), 4, true);
  std::copy(original.begin(), original.end(), universal.begin() + 32);
  check(reaweb::normalize_video_macho(universal) && universal == canonical);
}
