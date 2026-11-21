#if defined(__linux__) && defined(__x86_64__)
#include "video/layout.hpp"
#include "runtime/jsfx_transfer.hpp"
#include <link.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace reaweb {
namespace {
#include "layout_linux_rules.inc"

template<class T> T read(const unsigned char* p) {
  T value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

constexpr bool valid_rules() {
  for (const auto& rule : rules) {
    if (rule.mask_begin + rule.mask_count > std::size(operands) ||
        rule.ref_begin + rule.ref_count > std::size(refs)) return false;
    for (size_t i = rule.mask_begin; i < rule.mask_begin + rule.mask_count; ++i)
      if (size_t(operands[i]) + 4 > rule.size) return false;
    for (size_t i = rule.ref_begin; i < rule.ref_begin + rule.ref_count; ++i)
      if (refs[i].next < 4 || refs[i].offset + refs[i].next > rule.size) return false;
  }
  return true;
}
static_assert(valid_rules());

using Addresses = std::array<size_t, static_cast<size_t>(Role::count)>;
size_t get(const Addresses& addresses, Role role) { return addresses[static_cast<size_t>(role)]; }
bool bind(Addresses& addresses, Role role, int64_t value) {
  auto& address = addresses[static_cast<size_t>(role)];
  if (value <= 0 || (address && address != static_cast<size_t>(value))) return false;
  address = static_cast<size_t>(value);
  return true;
}

struct Image {
  const unsigned char* base;
  const ElfW(Phdr)* headers = nullptr;
  unsigned count = 0;

  bool contains(unsigned type, size_t rva, size_t bytes, int required, int forbidden = 0) const {
    for (unsigned i = 0; i < count; ++i) {
      const auto& h = headers[i];
      if (h.p_type != type || (h.p_flags & required) != unsigned(required) || (h.p_flags & forbidden) ||
          rva < h.p_vaddr || bytes > h.p_memsz) continue;
      if (rva - h.p_vaddr <= h.p_memsz - bytes) return true;
    }
    return false;
  }

  const ElfW(Phdr)* find(unsigned type) const {
    for (unsigned i = 0; i < count; ++i)
      if (headers[i].p_type == type) return &headers[i];
    return nullptr;
  }
};

bool detect(const Image& image, Addresses& found) {
  const auto* hdr = image.find(PT_GNU_EH_FRAME);
  if (!hdr || hdr->p_memsz < 12 || !image.contains(PT_LOAD, hdr->p_vaddr, hdr->p_memsz, PF_R, PF_W)) return false;
  const auto* table = image.base + hdr->p_vaddr;
  // Version 1, pcrel sdata4 .eh_frame, udata4 count, datarel sdata4 search table.
  if (std::memcmp(table, "\x01\x1b\x03\x3b", 4)) return false;
  const size_t count = read<uint32_t>(table + 8);
  if (count > (hdr->p_memsz - 12) / 8) return false;
  std::array<bool, std::size(rules)> matched{};
  JsfxTransfer::Bytes code;
  int64_t previous = -1;
  for (size_t i = 0; i < count; ++i) {
    const auto* entry = table + 12 + i * 8;
    const int64_t function = int64_t(hdr->p_vaddr) + read<int32_t>(entry);
    const int64_t fde = int64_t(hdr->p_vaddr) + read<int32_t>(entry + 4);
    if (function <= previous || fde <= 0 || !image.contains(PT_LOAD, fde, 16, PF_R, PF_W)) return false;
    previous = function;
    // Check the FDE's own PC-relative start and PC range against its index.
    const auto fde_size = read<uint32_t>(image.base + fde);
    if (fde_size < 12 || !image.contains(PT_LOAD, fde + 4, fde_size, PF_R, PF_W) ||
        fde + 8 + read<int32_t>(image.base + fde + 8) != function) return false;
    const size_t size = read<uint32_t>(image.base + fde + 12);
    for (size_t j = 0; j < std::size(rules); ++j) {
      const auto& rule = rules[j];
      if (size != rule.size) continue;
      if (!image.contains(PT_LOAD, function, size, PF_R | PF_X, PF_W)) return false;
      const auto* start = image.base + function;
      code.assign(start, start + size);
      for (size_t k = rule.mask_begin; k < rule.mask_begin + rule.mask_count; ++k)
        std::fill_n(code.begin() + operands[k], 4, 0);
      if (JsfxTransfer::sha256(code) != rule.digest) continue;
      if (matched[j] || !bind(found, rule.role, function)) return false;
      matched[j] = true;
      for (size_t k = rule.ref_begin; k < rule.ref_begin + rule.ref_count; ++k) {
        const auto& ref = refs[k];
        if (!bind(found, ref.target, function + ref.offset + ref.next + read<int32_t>(start + ref.offset))) return false;
      }
    }
  }
  if (std::find(matched.begin(), matched.end(), false) != matched.end()) return false;
  for (auto it = found.begin(); it != found.end(); ++it)
    if (!*it || std::find(found.begin(), it, *it) != it) return false;
  return true;
}

bool has_visibility_import(const Image& image, size_t slot) {
  // SWELL's relocated function registration table contains {name, &slot}.
  // Independently verify the name of the indirect call used to restore visibility.
  const auto* relro = image.find(PT_GNU_RELRO);
  if (!relro || !image.contains(PT_LOAD, relro->p_vaddr, relro->p_memsz, PF_R)) return false;
  constexpr char name[] = "IsWindowVisible";
  const auto base = reinterpret_cast<uintptr_t>(image.base);
  for (size_t i = 0; i + 2 * sizeof(uintptr_t) <= relro->p_memsz; i += sizeof(uintptr_t)) {
    const auto* pair = image.base + relro->p_vaddr + i;
    if (read<uintptr_t>(pair + sizeof(uintptr_t)) != base + slot) continue;
    const auto string = read<uintptr_t>(pair);
    if (string >= base && image.contains(PT_LOAD, string - base, sizeof(name), PF_R, PF_W) &&
        !std::memcmp(image.base + (string - base), name, sizeof(name))) return true;
  }
  return false;
}

bool select_layout(const Image& image, VideoBuild& result) {
  Addresses found{};
  if (!detect(image, found)) return false;
  const auto at = [&](Role role) { return get(found, role); };
  const auto flag = read<uint32_t>(image.base + at(Role::display) + display_operand);
  const auto setter = read<uint32_t>(image.base + at(Role::parameter) + parameter_operand);
  const auto size = read<uint32_t>(image.base + at(Role::factory) + factory_operand);
  if (!flag || flag != setter || size <= flag || size - flag != 0x130) return false;

  constexpr struct { Role role; size_t width; } fields[]{
    {Role::visible, 1}, {Role::window, 8}, {Role::frame, 8}, {Role::processed, 8},
    {Role::monitor_hash, 8}, {Role::frame_index, 8}, {Role::is_visible, 8},
    {Role::converter, 4}, {Role::codec_version, 1},
  };
  for (const auto& field : fields) {
    const auto offset = at(field.role);
    if (offset % field.width || !image.contains(PT_LOAD, offset, field.width, PF_R | PF_W, PF_X) ||
        image.contains(PT_GNU_RELRO, offset, 1, 0)) return false;
  }
  if (at(Role::converter) != at(Role::codec_version) + 4 || at(Role::window) != at(Role::visible) + 8 ||
      at(Role::monitor_hash) != at(Role::frame_index) + 8 || at(Role::processed) != at(Role::monitor_hash) + 8 ||
      at(Role::frame) != at(Role::processed) + 8 || !has_visibility_import(image, at(Role::is_visible))) return false;

  VideoBuild candidate{};
  candidate.display = at(Role::display);
  candidate.visible = at(Role::visible);
  candidate.window = at(Role::window);
  candidate.frame = at(Role::frame);
  candidate.processed = at(Role::processed);
  candidate.monitor_hash = at(Role::monitor_hash);
  candidate.query = at(Role::query);
  candidate.source_hash = at(Role::source_hash);
  candidate.cached = at(Role::cached);
  candidate.destroy_sources = at(Role::destroy_sources);
  candidate.frame_index = at(Role::frame_index);
  candidate.is_visible = at(Role::is_visible);
  candidate.transform = at(Role::transform);
  candidate.yuv_gamut = at(Role::yuv_gamut);
  candidate.converter = at(Role::converter);
  candidate.codec_version = at(Role::codec_version);
  candidate.sources_at = at(Role::sources_at);
  result = candidate;
  return true;
}
}

bool resolve_video_layout(const unsigned char* base, const std::filesystem::path&, VideoBuild& result) {
  if (!base) return false;
  Image image{base};
  dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) {
    auto& image = *static_cast<Image*>(data);
    if (reinterpret_cast<uintptr_t>(image.base) != info->dlpi_addr) return 0;
    image.headers = info->dlpi_phdr;
    image.count = info->dlpi_phnum;
    return 1;
  }, &image);
  return select_layout(image, result);
}
}
#endif
