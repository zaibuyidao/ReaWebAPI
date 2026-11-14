#include "video/fingerprint.hpp"
#include "video/builds.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <cstdint>
#include <set>

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
void registered_builds() {
  CHECK(reaweb::find_video_build("") == nullptr);
  CHECK(reaweb::find_video_build(std::string(64, '0')) == nullptr);
  std::set<std::string> fingerprints;
  for (const auto& build : reaweb::video_builds) {
    if (!build.checksum) continue;
    const std::string fingerprint(build.checksum);
    CHECK(fingerprint.size() == 64);
    CHECK(fingerprint.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(fingerprints.insert(fingerprint).second);
    CHECK(reaweb::find_video_build(fingerprint) == &build);
    CHECK(build.display && build.visible && build.window && build.frame && build.processed && build.monitor_hash);
    CHECK(build.query && build.source_hash && build.cached && build.destroy_sources && build.frame_index);
    CHECK(build.is_visible && build.transform && build.yuv_gamut && build.sources_at);
    auto changed = fingerprint;
    changed.back() = changed.back() == '0' ? '1' : '0';
    CHECK(reaweb::find_video_build(changed) == nullptr);
  }
#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
  CHECK(reaweb::find_video_build("b50bc6486f996acc2731acc6889217fdfb8f55dd8e6524746e7d7582ff705f59"));
#elif defined(__APPLE__) && defined(__aarch64__)
  CHECK(reaweb::find_video_build("318fef1c8d990840587b26cc58e2b7e9659844156b8578ba540e493c09f32ba0"));
#elif defined(__linux__) && defined(__x86_64__)
  CHECK(reaweb::find_video_build("f34a2e7634400dedfc70757e5daa7861988529e16ddcb2a3a85c5b72841e9535"));
  CHECK(reaweb::find_video_build("ef1bb206879f4a73f80550f551b98bb00b9493dd98648421c70c3d6eda3790ee"));
  CHECK(reaweb::find_video_build("29efc78f2099a22a5a5c3cd31c0136f31af0d14ad619901fd7d0e914a2a7f3fd"));
#else
  CHECK(fingerprints.empty());
#endif
}
}
void test_video_elf_metadata();
int main(int argc, char** argv) {
  try {
    if (argc > 1) {
      const bool check = std::string_view(argv[1]) == "--check";
      if (check && argc == 2) throw std::runtime_error("Usage: video_fingerprint_tests [--check] <module>...");
      bool supported = true;
      for (int i = check ? 2 : 1; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary);
        if (!file) throw std::runtime_error("Cannot open module");
        Bytes bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const auto digest = reaweb::video_module_fingerprint(std::move(bytes));
        CHECK(!digest.empty());
        std::cout << digest << "  " << argv[i];
        if (check) {
          const auto* build = reaweb::find_video_build(digest);
          std::cout << (build ? "  supported" : "  unsupported");
          supported = supported && build;
        }
        std::cout << '\n';
      }
      if (!supported) return 2;
    } else {
      registered_builds();
      windows_metadata();
      video_fingerprint_macho_tests();
      test_video_elf_metadata();
      std::cout << "Video build selection, fingerprint metadata, code/layout changes and malformed images passed\n";
    }
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
