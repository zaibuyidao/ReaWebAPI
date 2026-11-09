#include "video/fingerprint.hpp"
#include <cstdint>
#include <stdexcept>
#include <vector>

void test_video_elf_metadata() {
  using Bytes = std::vector<unsigned char>;
  const auto put = [](Bytes& b, size_t offset, uint64_t value, unsigned size = 4) {
    for (unsigned i = 0; i < size; ++i) b.at(offset + i) = static_cast<unsigned char>(value >> (8 * i));
  };
  const auto check = [](bool value) { if (!value) throw std::runtime_error("ELF fingerprint contract"); };
  Bytes original(0x300);
  original[0] = 0x7f; original[1] = 'E'; original[2] = 'L'; original[3] = 'F';
  original[4] = 2; original[5] = 1; original[6] = 1;
  put(original, 20, 1); put(original, 40, 0x80, 8);
  put(original, 52, 64, 2); put(original, 58, 64, 2); put(original, 60, 3, 2);
  put(original, 0xc4, 7); put(original, 0xd8, 0x200, 8); put(original, 0xe0, 36, 8);
  put(original, 0x104, 1); put(original, 0x118, 0x240, 8); put(original, 0x120, 32, 8);
  put(original, 0x200, 4); put(original, 0x204, 20); put(original, 0x208, 3);
  original[0x20c] = 'G'; original[0x20d] = 'N'; original[0x20e] = 'U';
  const auto digest = reaweb::video_module_fingerprint(original);
  check(digest.size() == 64);
  auto changed = original; changed[0x210] = 42;
  check(reaweb::video_module_fingerprint(changed) == digest);
  for (size_t at : {0x240u, 0x110u, 0x20cu}) {
    changed = original; changed[at] ^= 1;
    check(reaweb::video_module_fingerprint(changed) != digest);
  }
  for (size_t at : {40u, 0xd8u, 0xe0u, 0x200u, 0x204u}) {
    changed = original; put(changed, at, 0xffffffff);
    check(reaweb::video_module_fingerprint(changed).empty());
  }
  changed = original; changed.resize(0x220);
  check(reaweb::video_module_fingerprint(changed).empty());
}
