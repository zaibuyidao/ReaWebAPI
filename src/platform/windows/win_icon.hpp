#pragma once
#include <windows.h>
#include "runtime/icon.hpp"

namespace reaweb {
class WinIcon {
  HICON small_ = nullptr, large_ = nullptr;
  bool visible_ = true;
  static HICON create(const IconBitmap& image) {
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.size; info.bmiHeader.biHeight = -image.size;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    auto color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!color) throw Error("ICON_APPLY_FAILED", "Cannot allocate the native window icon");
    const size_t mask_stride = ((image.size + 15) / 16) * 2;
    std::vector<uint8_t> mask_bits(mask_stride * image.size);
    auto out = static_cast<uint8_t*>(pixels);
    for (int y = 0; y < image.size; ++y) for (int x = 0; x < image.size; ++x) {
      const auto index = (size_t(y) * image.size + x) * 4;
      const auto p = image.rgba.data() + index;
      out[index] = uint8_t((unsigned(p[2]) * p[3] + 127) / 255);
      out[index+1] = uint8_t((unsigned(p[1]) * p[3] + 127) / 255);
      out[index+2] = uint8_t((unsigned(p[0]) * p[3] + 127) / 255); out[index+3] = p[3];
      if (!p[3]) mask_bits[size_t(y) * mask_stride + x / 8] |= 0x80 >> (x % 8);
    }
    auto mask = CreateBitmap(image.size, image.size, 1, 1, mask_bits.data());
    ICONINFO icon{}; icon.fIcon = TRUE; icon.hbmColor = color; icon.hbmMask = mask;
    auto result = mask ? CreateIconIndirect(&icon) : nullptr;
    DeleteObject(color); if (mask) DeleteObject(mask);
    if (!result) throw Error("ICON_APPLY_FAILED", "Cannot create the native window icon");
    return result;
  }
public:
  ~WinIcon() { if (small_) DestroyIcon(small_); if (large_) DestroyIcon(large_); }
  void refresh(HWND window) {
    if (!window) return;
    const auto small_icon = visible_ ? small_ : nullptr, large_icon = visible_ ? large_ : nullptr;
    if (reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_SMALL, 0)) != small_icon)
      SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    if (reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_BIG, 0)) != large_icon)
      SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const bool hide_slot = !visible_ && !(style & WS_CHILD) && (style & WS_CAPTION);
    const auto next = hide_slot ? extended | WS_EX_DLGMODALFRAME : extended & ~LONG_PTR(WS_EX_DLGMODALFRAME);
    if (next != extended) {
      SetWindowLongPtrW(window, GWL_EXSTYLE, next);
      SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
  }
  void set_visible(HWND window, bool visible) { visible_ = visible; refresh(window); }
  void clear(HWND window) {
    auto previous_small = small_, previous_large = large_;
    small_ = large_ = nullptr;
    refresh(window);
    if (previous_small) DestroyIcon(previous_small); if (previous_large) DestroyIcon(previous_large);
  }
  void set(HWND window, const std::vector<IconBitmap>& images) {
    auto small_icon = create(images.front());
    HICON large_icon = nullptr;
    try { large_icon = create(images.back()); } catch (...) { DestroyIcon(small_icon); throw; }
    auto previous_small = small_, previous_large = large_;
    small_ = small_icon; large_ = large_icon;
    refresh(window);
    if (previous_small) DestroyIcon(previous_small); if (previous_large) DestroyIcon(previous_large);
  }
};
}
