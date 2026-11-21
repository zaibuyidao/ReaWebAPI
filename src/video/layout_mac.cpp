#include "video/layout.hpp"

#if defined(__APPLE__) && defined(__aarch64__)
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

namespace reaweb {
namespace {
struct Range {
  uint64_t start{}, size{};
  bool contains(uint64_t address, uint64_t width) const {
    return address >= start && width <= size && address - start <= size - width;
  }
};
struct Symbol {
  const char* name;
  size_t VideoBuild::*field;
  size_t width; // Zero denotes a function in __text.
  Range range{};
};
enum Entry { Display, Query, Hash, Cached, Destroy, Transform, Gamut, SourcesAt, Clear, Hook,
             Visible, Window, Frame, Processed, MonitorHash, FrameIndex, IsVisible, Count };
using Symbols = std::array<Symbol, Count>;

bool locate(const unsigned char* base, Symbols& symbols) {
  const auto* header = reinterpret_cast<const mach_header_64*>(base);
  if (!header || header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64 ||
      header->filetype != MH_DYLIB) return false;
  const segment_command_64* link = nullptr;
  const symtab_command* table = nullptr;
  Range text{}, globals{};
  auto* cursor = base + sizeof(*header);
  size_t remaining = header->sizeofcmds;
  for (uint32_t i = 0; i < header->ncmds; ++i) {
    if (remaining < sizeof(load_command)) return false;
    const auto* cmd = reinterpret_cast<const load_command*>(cursor);
    if (cmd->cmdsize < sizeof(*cmd) || cmd->cmdsize % 8 || cmd->cmdsize > remaining) return false;
    if (cmd->cmd == LC_SYMTAB) {
      if (table || cmd->cmdsize != sizeof(symtab_command)) return false;
      table = reinterpret_cast<const symtab_command*>(cmd);
    } else if (cmd->cmd == LC_SEGMENT_64) {
      if (cmd->cmdsize < sizeof(segment_command_64)) return false;
      const auto* seg = reinterpret_cast<const segment_command_64*>(cmd);
      if (seg->nsects > (cmd->cmdsize - sizeof(*seg)) / sizeof(section_64)) return false;
      if (!std::strncmp(seg->segname, "__LINKEDIT", 16)) {
        if (link || seg->initprot != VM_PROT_READ || seg->filesize > seg->vmsize) return false;
        link = seg;
      }
      const bool code = !std::strncmp(seg->segname, "__TEXT", 16) && !seg->vmaddr &&
                        seg->initprot == (VM_PROT_READ | VM_PROT_EXECUTE);
      const bool data = !std::strncmp(seg->segname, "__DATA", 16) &&
                        seg->initprot == (VM_PROT_READ | VM_PROT_WRITE);
      const auto* sections = reinterpret_cast<const section_64*>(seg + 1);
      for (uint32_t j = 0; j < seg->nsects; ++j) {
        const auto& s = sections[j];
        if (!Range{seg->vmaddr, seg->vmsize}.contains(s.addr, s.size)) return false;
        if (code && !std::strncmp(s.sectname, "__text", 16)) text = {s.addr, s.size};
        if (data && !std::strncmp(s.sectname, "__common", 16)) globals = {s.addr, s.size};
      }
    }
    cursor += cmd->cmdsize;
    remaining -= cmd->cmdsize;
  }
  if (remaining || !text.size || !globals.size || !table || !link) return false;
  const Range file{link->fileoff, link->filesize};
  if (!file.contains(table->symoff, uint64_t(table->nsyms) * sizeof(nlist_64)) ||
      !file.contains(table->stroff, table->strsize) || table->symoff % alignof(nlist_64)) return false;
  const auto* bytes = base + link->vmaddr;
  const auto* list = reinterpret_cast<const nlist_64*>(bytes + (table->symoff - file.start));
  const auto* strings = reinterpret_cast<const char*>(bytes + (table->stroff - file.start));
  for (uint32_t i = 0; i < table->nsyms; ++i) {
    const auto& sym = list[i];
    if ((sym.n_type & (N_STAB | N_TYPE)) != N_SECT) continue;
    const auto index = sym.n_un.n_strx;
    if (index >= table->strsize || !std::memchr(strings + index, 0, table->strsize - index)) return false;
    for (auto& wanted : symbols) {
      if (std::strcmp(strings + index, wanted.name)) continue;
      const size_t width = wanted.width ? wanted.width : sizeof(uint32_t);
      const auto region = wanted.width ? globals : text;
      if (wanted.range.size || sym.n_value % width || !region.contains(sym.n_value, width)) return false;
      wanted.range = {sym.n_value, wanted.width ? width : text.size - (sym.n_value - text.start)};
    }
  }
  for (auto& wanted : symbols) {
    if (!wanted.range.size) return false;
    if (wanted.width) continue;
    for (uint32_t i = 0; i < table->nsyms; ++i) {
      const auto& sym = list[i];
      if ((sym.n_type & (N_STAB | N_TYPE)) == N_SECT && sym.n_value > wanted.range.start)
        wanted.range.size = std::min(wanted.range.size, sym.n_value - wanted.range.start);
    }
  }
  return true;
}

int64_t signed_field(uint32_t value, unsigned bits) {
  return int64_t(value) - ((value >> (bits - 1)) ? int64_t{1} << bits : 0);
}

class Routine {
  const unsigned char* base_;
  Range range_;
  uint32_t word(size_t i) const {
    uint32_t w;
    std::memcpy(&w, base_ + range_.start + i, sizeof(w));
    return w;
  }
public:
  Routine(const unsigned char* image, const Symbol& symbol) : base_(image), range_(symbol.range) {}
  bool instructions(std::initializer_list<uint32_t> opcodes) const {
    for (size_t i = 0; i + opcodes.size() * 4 <= range_.size; i += 4)
      if (!std::memcmp(base_ + range_.start + i, opcodes.begin(), opcodes.size() * 4)) return true;
    return false;
  }
  bool calls(const Symbol& callee) const {
    for (size_t i = 0; i + 4 <= range_.size; i += 4) {
      const auto w = word(i);
      if ((w >> 26) == 0x25 && range_.start + i + 4 * signed_field(w & 0x3ffffff, 26) == callee.range.start) return true;
    }
    return false;
  }
  // LDR literal or ADRP followed by a load/store using the same page register.
  bool references(const Symbol& data, uint32_t access, bool indirect_call = false) const {
    for (size_t i = 0; i + 8 <= range_.size; i += 4) {
      const auto pc = range_.start + i;
      const auto w = word(i), next = word(i + 4);
      if (access == 0xf9400000 && (w & 0xff000000) == 0x58000000 &&
          pc + 4 * signed_field((w >> 5) & 0x7ffff, 19) == data.range.start &&
          (!indirect_call || next == (0xd63f0000u | ((w & 31) << 5)))) return true;
      if (indirect_call || (w & 0x9f000000) != 0x90000000 || (next & 0xffc00000) != access ||
          ((next >> 5) & 31) != (w & 31)) continue;
      const auto pages = signed_field((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21);
      if ((pc & ~uint64_t{4095}) + pages * 4096 + (((next >> 10) & 4095) << (next >> 30)) == data.range.start)
        return true;
    }
    return false;
  }
};
}
bool resolve_video_layout(const unsigned char* base, const std::filesystem::path&, VideoBuild& result) {
  Symbols s{{
    {"__Z17displayVideoFramev", &VideoBuild::display, 0},
    {"__Z15GetDisplayStateP15ownedSourceInstPxdS0_P10ReaProjectS3_19srclist_cache_indexP11WDL_PtrListI21PCM_source_video_baseE", &VideoBuild::query, 0},
    {"__ZN15ownedSourceInst8get_hashEP10ReaProject", &VideoBuild::source_hash, 0},
    {"__Z19getCachedVideoFrameyxbPxP10ReaProject", &VideoBuild::cached, 0},
    {"__ZN15ownedSourceInstD2Ev", &VideoBuild::destroy_sources, 0},
    {"__Z14transformFrameP10videoFramebiPPvPKc", &VideoBuild::transform, 0},
    {"__Z18get_proj_yuv_gamutP10ReaProject", &VideoBuild::yuv_gamut, 0},
    {"__Z25GetProjectSourceFromFrameP10ReaProjectP21PCM_source_video_basexP15ownedSourceInstdS4_19srclist_cache_indexP11WDL_PtrListIS1_E", &VideoBuild::sources_at, 0},
    {"__ZN15ownedSourceInst5clearEv", nullptr, 0},
    {"__Z15hookCommandProcii", nullptr, 0},
    {"_g_video_hwnd_visible", &VideoBuild::visible, 1},
    {"_g_video_hwnd", &VideoBuild::window, 8},
    {"_g_displaying_frame", &VideoBuild::frame, 8},
    {"_g_displaying_frame2", &VideoBuild::processed, 8},
    {"_g_displaying_frame2_hash", &VideoBuild::monitor_hash, 8},
    {"_g_displaying_frame_fridx", &VideoBuild::frame_index, 8},
    {"_IsWindowVisible", &VideoBuild::is_visible, 8},
  }};
  if (!locate(base, s)) return false;
  // The display cache is four consecutive 64-bit slots. The window handle
  // is immediately followed by its visibility flag. Both groups may relocate.
  if (s[Processed].range.start != s[Frame].range.start + 8 ||
      s[MonitorHash].range.start != s[Processed].range.start + 8 ||
      s[FrameIndex].range.start != s[MonitorHash].range.start + 8 ||
      s[Visible].range.start != s[Window].range.start + 8) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    for (size_t j = i + 1; j < s.size(); ++j) {
      const auto a = s[i].range, b = s[j].range;
      if (a.contains(b.start, 1) || b.contains(a.start, 1)) return false;
    }
  }
  const Routine display(base, s[Display]), hook(base, s[Hook]), query(base, s[Query]),
      destroy(base, s[Destroy]), clear(base, s[Clear]), transform(base, s[Transform]), gamut(base, s[Gamut]);
  if (!display.calls(s[Query]) || !display.calls(s[Hash]) || !display.calls(s[Cached]) ||
      !display.calls(s[Transform]) || !query.calls(s[SourcesAt]) || !query.calls(s[Clear]) ||
      !destroy.calls(s[Clear]) || !transform.calls(s[Gamut])) return false;
  if (!display.references(s[Frame], 0xf9400000) || !display.references(s[Processed], 0xf9400000) ||
      !display.references(s[MonitorHash], 0xf9000000) || !display.references(s[FrameIndex], 0xf9000000) ||
      !hook.references(s[Window], 0xf9400000) || !hook.references(s[Visible], 0x39400000) ||
      !hook.references(s[IsVisible], 0xf9400000, true)) return false;
  // Source ownership flags at +0x18/+0x19 and used byte count at +0xc.
  if (!clear.instructions({0x39406008, 0x34000068, 0x39406669, 0x340004c9, 0xb9400e69}) ||
      // Frame format at +0x24, width/height at +0x1c, and the RGBA conversion path.
      !transform.instructions({0xb9402668, 0x6b02011f}) ||
      !transform.instructions({0x2943a668, 0x5288482a, 0x72aa48ea, 0x6b0a02df}) ||
      // Project YUV gamut loads the flags and maps reserved matrix 3 to 1.
      !gamut.instructions({0x79400408, 0x12000508, 0x71000d1f, 0x1a9f1500})) return false;
  VideoBuild layout{};
  for (const auto& entry : s) if (entry.field) layout.*entry.field = entry.range.start;
  result = layout;
  return true;
}
}
#endif
