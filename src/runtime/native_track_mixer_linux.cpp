#if defined(__linux__) && defined(__x86_64__)
#include "runtime/native_track_cache.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <link.h>
#include <pthread.h>
#include <string_view>

namespace reaweb {
namespace {
using Bytes = std::string_view;

struct InstructionBlock {
  size_t offset, length;
  uint64_t hash;
};

uint64_t instruction_hash(Bytes code) noexcept {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char byte : code) hash = (hash ^ byte) * 0x100000001b3ULL;
  return hash;
}

// These are instruction offsets within the 7.86 render routine, measured from
// the cache lock below. Every field displacement used by the tap is part of a
// checked instruction. A changed compiler layout requires revalidation.
constexpr InstructionBlock cache_blocks[]{
  {0x0f7, 73, 0x92b7ecec50ce19de}, // Write loop, time, frames, valid frames, channels and next.
  {0x146, 16, 0x463df61fa7618b6d}, // Load track cache list 1 (+0x2410).
  {0x156, 59, 0x89f868ecc5b0bf2c}, // Walk the list, link node, recover the saved lock.
  {0x404, 44, 0xe9cc9592fb3bf49d}, // Cached PCM: buffer bytes, silence, channels, pointer, mixer arguments.
  {0x789, 36, 0x353b242ce473a2b8}, // Realtime path passes its gain array to the same mixer.
  {0x8d3, 7,  0xdebf3d0f6a344403}, // Store the head of the previously empty list.
  {0x9d4, 69, 0x99d811cb3eb39682}, // Read and advance both previous gains (+0xe8/+0xf0).
};

template<size_t N>
size_t unique_offset(Bytes code, const char (&signature)[N]) noexcept {
  const Bytes needle(signature, N - 1);
  const auto found = code.find(needle);
  if (found == Bytes::npos || code.find(needle, found + 1) != Bytes::npos) return Bytes::npos;
  return found;
}

// rel32 operand of a direct call/jump, or of a RIP-relative memory access.
int64_t relative_target(Bytes code, size_t operand) noexcept {
  int32_t delta;
  std::memcpy(&delta, code.data() + operand, sizeof(delta));
  return static_cast<int64_t>(operand + sizeof(delta)) + delta;
}

bool mixer_abi(Bytes code) noexcept {
  // Complete InterleaveCopySamples from the 7.86 x86_64 reference, with
  // RIP-relative constants and external memcpy/memset rel32 operands zeroed.
  std::array<char, 0x866> normalized;
  if (code.size() < normalized.size()) return false;
  std::memcpy(normalized.data(), code.data(), normalized.size());
  for (size_t offset : {0xa3, 0x104, 0x111, 0x260, 0x2bf, 0x3ea, 0x4b9, 0x4cf, 0x4df, 0x51e, 0x665})
    std::memset(normalized.data() + offset, 0, 4);
  return instruction_hash({normalized.data(), normalized.size()}) == 0xca1b7cdf68bc5bd7;
}

struct Resolved {
  const char* mixer = nullptr;
  const char* lock_plt = nullptr;
  const char* unlock_plt = nullptr;
};

Resolved resolve(Bytes code) noexcept {
  // lea rax,[rbx+0x33b0]; pass and save the cache lock for the matching unlock.
  const auto lock = unique_offset(code, "\x48\x8d\x83\xb0\x33\x00\x00\x48\x89\xc7\x48\x89\x85\x40\xff\xff\xff\xe8");
  if (lock == Bytes::npos || code.size() - lock < 0xa1d) return {};
  const Bytes render = code.substr(lock);
  for (const auto& block : cache_blocks) {
    if (instruction_hash(render.substr(block.offset, block.length)) != block.hash) return {};
  }
  const auto mix = unique_offset(code, "\x41\x57\x41\x56\x41\x55\x41\x54\x55\x53\x48\x89\xfd\x48\x83\xec\x08"
                                       "\x8b\x7e\x14\x44\x8b\x5e\x1c\x44\x8b\x54\x24\x40");
  if (mix == Bytes::npos || !mixer_abi(code.substr(mix))) return {};
  // Both mixer calls must agree, and the gain block must branch back to the
  // cached call. The empty-list branch must lead to the checked head store.
  if (relative_target(render, 0x9d4 + 69) != 0x404 ||
      relative_target(render, 0x146 + 12) != 0x8d3 ||
      relative_target(code, lock + 0x404 + 44) != static_cast<int64_t>(mix) ||
      relative_target(code, lock + 0x789 + 36) != static_cast<int64_t>(mix)) return {};
  if (static_cast<unsigned char>(render[0x191]) != 0xe8) return {};
  const auto lock_stub = relative_target(code, lock + 18);
  const auto unlock_stub = relative_target(code, lock + 0x192);
  const auto valid_stub = [&](int64_t at) {
    return at >= 0 && static_cast<size_t>(at) <= code.size() - 6 &&
           !std::memcmp(code.data() + at, "\xff\x25", 2);
  };
  if (!valid_stub(lock_stub) || !valid_stub(unlock_stub)) return {};
  return {code.data() + mix, code.data() + lock_stub, code.data() + unlock_stub};
}

bool calls_mutex(const dl_phdr_info& image, const char* plt, int (*function)(pthread_mutex_t*)) noexcept {
  const auto slot = reinterpret_cast<uintptr_t>(plt) + relative_target({plt, 6}, 2);
  for (int i = 0; i < image.dlpi_phnum; ++i) {
    const auto& load = image.dlpi_phdr[i];
    const auto base = image.dlpi_addr + load.p_vaddr;
    if (load.p_type != PT_LOAD || !(load.p_flags & PF_R) || slot < base ||
        load.p_memsz < sizeof(function) || slot - base > load.p_memsz - sizeof(function)) continue;
    decltype(function) resolved;
    std::memcpy(&resolved, reinterpret_cast<const void*>(slot), sizeof(resolved));
    return resolved == function;
  }
  return false;
}

int inspect_host(dl_phdr_info* image, size_t, void* output) {
  if (*image->dlpi_name) return 0;
  Bytes text;
  for (int i = 0; i < image->dlpi_phnum; ++i) {
    const auto& load = image->dlpi_phdr[i];
    if (load.p_type != PT_LOAD || !(load.p_flags & PF_X)) continue;
    if (!text.empty() || !(load.p_flags & PF_R) || load.p_filesz > load.p_memsz) return 1;
    text = {reinterpret_cast<const char*>(image->dlpi_addr + load.p_vaddr), load.p_filesz};
  }
  const auto found = resolve(text);
  if (found.mixer && calls_mutex(*image, found.lock_plt, pthread_mutex_lock) &&
      calls_mutex(*image, found.unlock_plt, pthread_mutex_unlock)) {
    *static_cast<NativeTrackCacheLayout*>(output) = {
      reinterpret_cast<NativeTrackCacheLayout::Mix>(found.mixer), 0x33b0, 0x2410, 0xe8,
    };
  }
  return 1;
}
}
NativeTrackCacheLayout native_track_cache_layout() noexcept {
  NativeTrackCacheLayout layout{};
  dl_iterate_phdr(inspect_host, &layout);
  return layout;
}
}
#endif
