#include "runtime/native_track_mixer.hpp"
#include "runtime/native_track_cache.hpp"

#if REAWEB_TRACK_WINDOWS_X64
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace reaweb {
namespace {
template<class T> T read(const void* address) {
  T value;
  std::memcpy(&value, address, sizeof(value));
  return value;
}
// Fingerprint relocation-free instructions within the three REAPER x64 routines.
// Every function must also have its own unwind entry and the expected call edges.
bool instructions(const unsigned char* p, size_t size, uint64_t expected) noexcept {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * 0x100000001b3ULL;
  return hash == expected;
}
bool resolve_mixer(unsigned char* image, uint32_t& result) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return false;
  const auto size = nt->OptionalHeader.SizeOfImage;
  const auto& table = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (!table.Size || table.VirtualAddress >= size || table.Size > size - table.VirtualAddress ||
      table.Size % sizeof(RUNTIME_FUNCTION)) return false;
  const auto* functions = reinterpret_cast<const RUNTIME_FUNCTION*>(image + table.VirtualAddress);
  const auto* end = functions + table.Size / sizeof(RUNTIME_FUNCTION);
  const auto section = IMAGE_FIRST_SECTION(nt);
  const auto text_end = section + nt->FileHeader.NumberOfSections;
  auto code = std::find_if(section, text_end, [](const IMAGE_SECTION_HEADER& s) {
    return std::memcmp(s.Name, ".text\0\0\0", 8) == 0 && (s.Characteristics & IMAGE_SCN_MEM_EXECUTE);
  });
  if (code == text_end || code->VirtualAddress >= size || code->Misc.VirtualSize > size - code->VirtualAddress) return false;
  auto in_code = [&](const RUNTIME_FUNCTION& f, uint32_t length) {
    return f.BeginAddress >= code->VirtualAddress && f.EndAddress <= code->VirtualAddress + code->Misc.VirtualSize &&
           f.EndAddress > f.BeginAddress && f.EndAddress - f.BeginAddress == length;
  };
  auto callee = [&](uint32_t call, uint32_t length) -> uint32_t {
    if (image[call] != 0xe8) return 0;
    const auto address = int64_t(call) + 5 + read<int32_t>(image + call + 1);
    auto f = std::lower_bound(functions, end, address, [](const RUNTIME_FUNCTION& r, int64_t key) {
      return r.BeginAddress < key;
    });
    return f != end && f->BeginAddress == address && in_code(*f, length) ? f->BeginAddress : 0;
  };
  for (auto f = functions; f != end; ++f) {
    if (!in_code(*f, 26458)) continue;
    const auto playback = f->BeginAddress + 0x25f8;
    if (!instructions(image + playback - 16, 16, 0xff8e14252bf59759ULL) ||
        !instructions(image + playback + 5, 22, 0x999d8822ba947e06ULL)) continue;
    const auto render = callee(playback, 5429);
    // Frame setup, track register, timestamp register and cached owner lock slot.
    if (!render || !instructions(image + render, 118, 0x4ff287514eab01caULL) ||
        !instructions(image + render + 0x145, 23, 0x1be8d8119c8472d8ULL)) continue;
    const uint32_t cached = render + 0x68a, realtime = render + 0x117f;
    if (!instructions(image + cached - 37, 37, 0x8989f37fa136f24fULL) ||
        !instructions(image + cached + 5, 20, 0x81b8838c43d550baULL) ||
        !instructions(image + realtime - 42, 42, 0xf9a5b2b145eb563cULL) ||
        !instructions(image + realtime + 5, 23, 0x286a3c79c4ec6d07ULL)) continue;
    const auto mix = callee(cached, 1340);
    // Both signal paths use the same sample, stride, destination and gain layout.
    if (!mix || mix != callee(realtime, 1340) ||
        !instructions(image + mix, 196, 0xc688cb55b0c75df0ULL)) continue;
    if (!instructions(image + render + 0x573, 165, 0x12ed8e73ef5eb09fULL)) continue;
    if (!instructions(image + render + 0x649, 65, 0xf3ffc2bbf31eaed1ULL)) continue;
    if (!instructions(image + render + 0x12fe, 45, 0x4480a3e7118dff27ULL)) continue;
    if (!instructions(image + render + 0x1344, 34, 0xf4decd7b8e6ec267ULL)) continue;
    if (!instructions(image + render + 0x1413, 8, 0xc7bc4efaac57e727ULL)) continue;
    const auto* op = image + render + 0x165;
    if (op[0] != 0xff || op[1] != 0x15) continue;
    const auto slot = static_cast<int64_t>(render) + 0x16b + read<int32_t>(op + 2);
    if (slot < 0 || uint64_t(slot) + sizeof(void*) > size ||
        read<void*>(image + slot) != GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "EnterCriticalSection")) continue;
    if (result) return false;
    result = mix;
  }
  return result != 0;
}
}

NativeTrackCacheLayout native_track_cache_layout() noexcept {
  auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
  uint32_t mix = 0;
  if (!image || !resolve_mixer(image, mix)) return {};
  return {reinterpret_cast<NativeTrackCacheLayout::Mix>(image + mix), 0x33b0, 0x2410, 0xe8};
}
}
#endif
