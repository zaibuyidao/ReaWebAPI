#pragma once
#include <windows.h>
#include "runtime/icon.hpp"

namespace reaweb {
class WinIcon {
  HICON small_ = nullptr, large_ = nullptr;
  bool visible_ = true;
  HWND dock_ = nullptr;
  HICON dock_small_ = nullptr, dock_large_ = nullptr;
  LONG_PTR dock_frame_ = 0;
  static constexpr auto dock_owner = L"ReaWebAPI.DockIcon";
  static constexpr LONG_PTR dock_frame_mask = WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW;
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
  static void apply(HWND window, HICON small_icon, HICON large_icon, LONG_PTR frame, LONG_PTR mask) {
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const auto next = (extended & ~mask) | frame;
    if (next != extended) SetWindowLongPtrW(window, GWL_EXSTYLE, next);
    if (reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_SMALL, 0)) != small_icon)
      SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    if (reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_BIG, 0)) != large_icon)
      SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
    if (next != extended) {
      // Refresh the cached tool-window caption so the Docker actually draws its icon.
      if ((next ^ extended) & WS_EX_TOOLWINDOW) SendMessageW(window, WM_THEMECHANGED, 0, 0);
      SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
  }
  void release_dock() {
    const auto previous = dock_; dock_ = nullptr;
    if (previous && GetPropW(previous, dock_owner) == this) {
      RemovePropW(previous, dock_owner);
      apply(previous, dock_small_, dock_large_, dock_frame_, dock_frame_mask);
    }
  }
public:
  ~WinIcon() { release_dock(); if (small_) DestroyIcon(small_); if (large_) DestroyIcon(large_); }
  void refresh(HWND window, HWND dock = nullptr) {
    if (!small_ && !large_ && visible_) dock = nullptr;
    if (dock_ != dock || (dock_ && GetPropW(dock_, dock_owner) != this)) release_dock();
    if (dock && !dock_) {
      // A floating Docker is shared. Transfer ownership before saving its original presentation.
      if (auto previous = static_cast<WinIcon*>(GetPropW(dock, dock_owner))) previous->release_dock();
      dock_small_ = reinterpret_cast<HICON>(SendMessageW(dock, WM_GETICON, ICON_SMALL, 0));
      dock_large_ = reinterpret_cast<HICON>(SendMessageW(dock, WM_GETICON, ICON_BIG, 0));
      dock_frame_ = GetWindowLongPtrW(dock, GWL_EXSTYLE) & dock_frame_mask;
      if (SetPropW(dock, dock_owner, this)) dock_ = dock;
    }
    const auto small_icon = visible_ ? small_ : nullptr, large_icon = visible_ ? large_ : nullptr;
    if (window) {
      const auto style = GetWindowLongPtrW(window, GWL_STYLE);
      const bool hide_slot = !visible_ && !(style & WS_CHILD) && (style & WS_CAPTION);
      apply(window, small_icon, large_icon, hide_slot ? WS_EX_DLGMODALFRAME : 0, WS_EX_DLGMODALFRAME);
    }
    if (dock_) apply(dock_, small_icon, large_icon,
      visible_ ? 0 : (dock_frame_ | WS_EX_DLGMODALFRAME), dock_frame_mask);
  }
  void set_visible(HWND window, bool visible, HWND dock = nullptr) { visible_ = visible; refresh(window, dock); }
  void clear(HWND window, HWND dock = nullptr) {
    auto previous_small = small_, previous_large = large_;
    small_ = large_ = nullptr;
    refresh(window, dock);
    if (previous_small) DestroyIcon(previous_small); if (previous_large) DestroyIcon(previous_large);
  }
  void set(HWND window, const std::vector<IconBitmap>& images, HWND dock = nullptr) {
    auto small_icon = create(images.front());
    HICON large_icon = nullptr;
    try { large_icon = create(images.back()); } catch (...) { DestroyIcon(small_icon); throw; }
    auto previous_small = small_, previous_large = large_;
    small_ = small_icon; large_ = large_icon;
    refresh(window, dock);
    if (previous_small) DestroyIcon(previous_small); if (previous_large) DestroyIcon(previous_large);
  }
};
}
