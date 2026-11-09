#include "video/fingerprint.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <cstdint>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::to_string(__LINE__) + ": " + #x); } while (0)
void video_fingerprint_macho_tests();
namespace {
using Bytes = std::vector<unsigned char>;
void put(Bytes& b, size_t at, uint64_t value, unsigned size = 4) {
  for (unsigned i = 0; i < size; ++i) b.at(at + i) = static_cast<unsigned char>(value >> (8 * i));
}
Bytes pe() {
  Bytes b(0x800, 0);
  put(b, 0, 0x5a4d, 2); put(b, 0x3c, 0x80);
  put(b, 0x80, 0x4550); put(b, 0x84, 0x8664, 2); put(b, 0x86, 1, 2);
  put(b, 0x94, 240, 2); put(b, 0x98, 0x20b, 2); put(b, 0x98 + 108, 16);
  put(b, 0x188 + 12, 0x1000); put(b, 0x188 + 16, 0x600); put(b, 0x188 + 20, 0x200);
  put(b, 0x98 + 112, 0x1100); put(b, 0x98 + 116, 40);
  put(b, 0x98 + 160, 0x1200); put(b, 0x98 + 164, 28);
  put(b, 0x40c, 2); put(b, 0x410, 32); put(b, 0x418, 0x500);
  put(b, 0x500, 0x53445352);
  return b;
}
void windows_metadata() {
  const auto original = pe();
  const auto digest = reaweb::video_module_fingerprint(original);
  CHECK(digest.size() == 64);
  for (size_t field : {0x88, 0xd8, 0x304, 0x404, 0x504, 0x514}) {
    auto changed = original; put(changed, field, 0x12345678);
    CHECK(reaweb::video_module_fingerprint(changed) == digest);
  }
  for (size_t field : {0x220, 0x600, 0x194, 0x310, 0x518}) {
    auto changed = original; changed[field] ^= 1;
    CHECK(reaweb::video_module_fingerprint(changed) != digest);
  }
  for (size_t field : {0x3c, 0x198, 0x19c, 0x138, 0x418, 0x410}) {
    auto changed = original; put(changed, field, 0xffffffff);
    CHECK(reaweb::video_module_fingerprint(changed).empty());
  }
  for (size_t size : {0u, 63u, 255u, 0x400u}) {
    auto changed = original; changed.resize(size);
    CHECK(reaweb::video_module_fingerprint(changed).empty());
  }
}
}
void test_video_elf_metadata();
int main(int argc, char** argv) {
  try {
    if (argc > 1) {
      for (int i = 1; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary);
        if (!file) throw std::runtime_error("Cannot open module");
        Bytes bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const auto digest = reaweb::video_module_fingerprint(std::move(bytes));
        CHECK(!digest.empty());
        std::cout << digest << "  " << argv[i] << '\n';
      }
    } else {
      windows_metadata();
      video_fingerprint_macho_tests();
      test_video_elf_metadata();
      std::cout << "Video fingerprint metadata, code/layout changes and malformed images passed\n";
    }
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
