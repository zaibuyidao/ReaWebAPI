#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
#include "video/layout.hpp"
#include "runtime/jsfx_transfer.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace reaweb {
namespace {
#include "layout_win_rules.inc"

template<class T> T read_value(const unsigned char* address) {
  T value;
  std::memcpy(&value, address, sizeof(value));
  return value;
}

constexpr bool valid_rules() {
  for (const auto& rule : rules) {
    if (rule.first_mask + rule.mask_count > std::size(masks) ||
        rule.first_reference + rule.reference_count > std::size(references)) return false;
    for (size_t j = rule.first_mask; j < rule.first_mask + rule.mask_count; ++j)
      if (!masks[j].bytes || masks[j].offset + masks[j].bytes > rule.size) return false;
    for (size_t j = rule.first_reference; j < rule.first_reference + rule.reference_count; ++j)
      if (references[j].next < 4 || references[j].offset + references[j].next > rule.size) return false;
  }
  return true;
}
static_assert(valid_rules());

struct Image {
  const unsigned char* base;
  size_t size = 0;
  const IMAGE_SECTION_HEADER* sections = nullptr;
  unsigned section_count = 0;
  const RUNTIME_FUNCTION* functions = nullptr;
  size_t function_count = 0;

  bool contains(size_t offset, size_t bytes) const {
    return offset <= size && bytes <= size - offset;
  }
  // A PE section may span discarded, guard or no-access pages in a loaded DLL.
  bool readable(size_t offset, size_t bytes) const {
    if (!contains(offset, bytes)) return false;
    while (bytes) {
      MEMORY_BASIC_INFORMATION region{};
      if (!VirtualQuery(base + offset, &region, sizeof(region)) || region.AllocationBase != base ||
          region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
      const auto protection = region.Protect & 0xff;
      if (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
          protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
          protection != PAGE_EXECUTE_WRITECOPY) return false;
      const auto used = reinterpret_cast<uintptr_t>(base + offset) - reinterpret_cast<uintptr_t>(region.BaseAddress);
      if (used >= region.RegionSize) return false;
      const auto chunk = (std::min)(bytes, region.RegionSize - used);
      bytes -= chunk;
      offset += chunk;
    }
    return true;
  }
  DWORD permissions(size_t offset, size_t bytes) const {
    if (!contains(offset, bytes)) return 0;
    for (unsigned i = 0; i < section_count; ++i) {
      const auto& section = sections[i];
      if (offset < section.VirtualAddress) continue;
      const auto within = offset - section.VirtualAddress;
      if (within <= section.Misc.VirtualSize && bytes <= section.Misc.VirtualSize - within)
        return section.Characteristics;
    }
    return 0;
  }
  bool parse() {
    MODULEINFO module{};
    if (!base || !K32GetModuleInformation(GetCurrentProcess(),
        reinterpret_cast<HMODULE>(const_cast<unsigned char*>(base)), &module, sizeof(module)) ||
        module.lpBaseOfDll != base) return false;
    size = module.SizeOfImage;
    if (!readable(0, sizeof(IMAGE_DOS_HEADER))) return false;
    const auto dos = read_value<IMAGE_DOS_HEADER>(base);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        !readable(dos.e_lfanew, sizeof(IMAGE_NT_HEADERS64))) return false;
    const auto nt = read_value<IMAGE_NT_HEADERS64>(base + dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage != size ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return false;
    const size_t table = size_t(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    section_count = nt.FileHeader.NumberOfSections;
    if (!section_count || section_count > 96 || !readable(table, section_count * sizeof(*sections))) return false;
    sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(base + table);
    const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!directory.Size || directory.Size % sizeof(RUNTIME_FUNCTION) ||
        !(permissions(directory.VirtualAddress, directory.Size) & IMAGE_SCN_MEM_READ) ||
        !readable(directory.VirtualAddress, directory.Size)) return false;
    functions = reinterpret_cast<const RUNTIME_FUNCTION*>(base + directory.VirtualAddress);
    function_count = directory.Size / sizeof(RUNTIME_FUNCTION);
    return true;
  }
};

using Addresses = std::array<size_t, static_cast<size_t>(Role::count)>;
size_t address(const Addresses& values, Role role) { return values[static_cast<size_t>(role)]; }

bool detect(const Image& image, Addresses& found) {
  const auto bind = [&](Role role, size_t rva) {
    auto& previous = found[static_cast<size_t>(role)];
    if (!rva || !image.contains(rva, 1) || (previous && previous != rva)) return false;
    previous = rva;
    return true;
  };
  JsfxTransfer::Bytes code;
  for (const auto& rule : rules) {
    size_t hit = 0;
    for (size_t i = 0; i < image.function_count; ++i) {
      const auto& function = image.functions[i];
      const auto start = function.BeginAddress;
      if (function.EndAddress <= start || function.EndAddress - start != rule.size ||
          !(image.permissions(start, rule.size) & IMAGE_SCN_MEM_EXECUTE)) continue;
      if (!image.readable(start, rule.size)) return false;
      code.assign(image.base + start, image.base + function.EndAddress);
      for (size_t j = rule.first_mask; j < rule.first_mask + rule.mask_count; ++j)
        std::fill_n(code.begin() + masks[j].offset, masks[j].bytes, 0);
      if (JsfxTransfer::sha256(code) != rule.digest) continue;
      if (hit) return false;
      hit = start;
    }
    if (!bind(rule.role, hit)) return false;
    for (size_t j = rule.first_reference; j < rule.first_reference + rule.reference_count; ++j) {
      const auto& ref = references[j];
      const auto operand = hit + ref.offset;
      const int64_t target = static_cast<int64_t>(operand + ref.next) + read_value<int32_t>(image.base + operand);
      if (target <= 0 || !bind(ref.target, static_cast<size_t>(target))) return false;
    }
  }
  for (auto it = found.begin(); it != found.end(); ++it)
    if (!*it || std::find(found.begin(), it, *it) != it) return false;
  for (const auto& pair : adjacent)
    if (address(found, pair.before) + pair.size != address(found, pair.after)) return false;
  return true;
}

bool validate(const Image& image, const Addresses& found) {
  const auto rva = [&](Role role) { return address(found, role); };
  // The display and parameter setter both mark the same VIDEOEFFECT member.
  // The factory independently supplies the object's allocation size.
  const auto flag = read_value<int32_t>(image.base + rva(Role::display) + display_flag_operand);
  const auto setter = read_value<int32_t>(image.base + rva(Role::set_effect_parameter) + set_effect_parameter_flag_operand);
  const auto size = read_value<uint32_t>(image.base + rva(Role::effect_factory) + effect_size_operand);
  if (flag <= 0 || flag != setter || static_cast<uint32_t>(flag) >= size ||
      size - static_cast<uint32_t>(flag) != effect_flag_tail_distance) return false;

  constexpr struct { Role role; size_t width; } globals[] = {
    {Role::visible, 1}, {Role::window, 8}, {Role::frame, 8}, {Role::processed, 8},
    {Role::monitor_hash, 8}, {Role::frame_index, 8}, {Role::converter, 4},
    {Role::codec_version, 1}, {Role::renderer, 8}, {Role::video_colorspace, 8}
  };
  for (const auto& field : globals) {
    const auto slot = rva(field.role);
    const auto flags = image.permissions(slot, field.width);
    if (!(flags & IMAGE_SCN_MEM_WRITE) || (flags & IMAGE_SCN_MEM_EXECUTE) ||
        slot % field.width || !image.readable(slot, field.width)) return false;
  }
  if (rva(Role::converter) != rva(Role::codec_version) + 4 ||
      rva(Role::monitor_hash) != rva(Role::frame_index) + 8 || rva(Role::processed) != rva(Role::monitor_hash) + 8 ||
      rva(Role::frame) != rva(Role::processed) + 8 || rva(Role::renderer) != rva(Role::frame) + 8) return false;

  const auto visible = rva(Role::is_visible), table = rva(Role::directdraw_vtable);
  const auto valid_table = [&](size_t offset, size_t bytes) {
    const auto flags = image.permissions(offset, bytes);
    return offset % sizeof(void*) == 0 && (flags & IMAGE_SCN_MEM_READ) && !(flags & IMAGE_SCN_MEM_EXECUTE) &&
           image.readable(offset, bytes);
  };
  if (!valid_table(visible, sizeof(void*)) || !valid_table(table, 9 * sizeof(void*))) return false;
  const auto is_visible = read_value<FARPROC>(image.base + visible);
  if (!is_visible || is_visible != GetProcAddress(GetModuleHandleW(L"user32.dll"), "IsWindowVisible")) return false;
  const auto vtable = read_value<std::array<uintptr_t, 9>>(image.base + table);
  if (vtable[3] != vtable[6] || vtable[8]) return false;
  const auto base = reinterpret_cast<uintptr_t>(image.base);
  for (size_t i = 0; i < 8; ++i)
    if (vtable[i] < base || !(image.permissions(vtable[i] - base, 1) & IMAGE_SCN_MEM_EXECUTE)) return false;
  return true;
}
}

bool resolve_video_layout(const unsigned char* base, const std::filesystem::path&, VideoBuild& result) {
  Image image{base};
  Addresses found{};
  if (!image.parse() || !detect(image, found) || !validate(image, found)) return false;
  VideoBuild candidate{};
  candidate.display = address(found, Role::display);
  candidate.visible = address(found, Role::visible);
  candidate.window = address(found, Role::window);
  candidate.frame = address(found, Role::frame);
  candidate.processed = address(found, Role::processed);
  candidate.monitor_hash = address(found, Role::monitor_hash);
  candidate.query = address(found, Role::query);
  candidate.source_hash = address(found, Role::source_hash);
  candidate.cached = address(found, Role::cached);
  candidate.destroy_sources = address(found, Role::destroy_sources);
  candidate.frame_index = address(found, Role::frame_index);
  candidate.is_visible = address(found, Role::is_visible);
  candidate.transform = address(found, Role::transform);
  candidate.yuv_gamut = address(found, Role::yuv_gamut);
  candidate.converter = address(found, Role::converter);
  candidate.codec_version = address(found, Role::codec_version);
  candidate.sources_at = address(found, Role::sources_at);
  candidate.renderer = address(found, Role::renderer);
  candidate.directdraw_vtable = address(found, Role::directdraw_vtable);
  candidate.video_colorspace = address(found, Role::video_colorspace);
  result = candidate;
  return true;
}
}
#endif
