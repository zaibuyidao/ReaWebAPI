#include "runtime/icon.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#ifdef _WIN32
#include "platform/windows/win_icon.hpp"
#endif
using namespace reaweb;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
void put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) bytes.at(at + i) = uint8_t(value >> (8 * i));
}
IconSource svg(const std::string& body) { return {".svg", {body.begin(), body.end()}}; }
void rejects(const IconSource& source) {
  try { render_icon(source, {16}); } catch (const Error& error) { CHECK(error.code == "ICON_INVALID"); return; }
  throw std::runtime_error("Invalid icon accepted");
}
std::vector<uint8_t> container(const std::vector<uint8_t>& frame, int size) {
  std::vector<uint8_t> bytes(22);
  bytes[2] = 1; bytes[4] = 1; bytes[6] = bytes[7] = uint8_t(size); bytes[10] = 1; bytes[12] = 32;
  put32(bytes, 14, uint32_t(frame.size())); put32(bytes, 18, 22);
  bytes.insert(bytes.end(), frame.begin(), frame.end()); return bytes;
}
int main() {
  try {
    auto source = svg("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 20 10'><rect width='20' height='10' fill='#ff0000' fill-opacity='.5'/></svg>");
    auto images = render_icon(source, {16, 32, 48, 64});
    auto page = icon_from_page({{"format", ".svg"}, {"bytes", encode_binary(reinterpret_cast<const char*>(source.bytes.data()), source.bytes.size())}});
    CHECK(page->bytes == source.bytes && page->format == source.format);
    try { icon_from_page({{"format", ".gif"}, {"bytes", encode_binary("GIF", 3)}}); throw std::runtime_error("Unsupported favicon accepted"); }
    catch (const Error& error) { CHECK(error.code == "ICON_FORMAT"); }
    try { icon_from_page({{"format", ".png"}, {"bytes", {{"__reawebBytes", std::string(6 * 1024 * 1024, 'A')}}}}); throw std::runtime_error("Oversize favicon accepted"); }
    catch (const Error& error) { CHECK(error.code == "ICON_LIMIT"); }
    for (auto& image : images) {
      CHECK(image.rgba.size() == size_t(image.size * image.size * 4));
      CHECK(image.rgba[3] == 0);
      auto center = size_t(image.size / 2 * image.size + image.size / 2) * 4;
      CHECK(image.rgba[center] >= 254 && image.rgba[center + 1] == 0);
      CHECK(image.rgba[center + 3] >= 127 && image.rgba[center + 3] <= 128);
    }
    // A 1x1 red RGBA PNG.
    const std::vector<uint8_t> png = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,0,0,0,13,73,68,65,84,120,156,99,248,207,192,240,31,0,5,0,1,255,137,153,61,29,0,0,0,0,73,69,78,68,174,66,96,130};
    auto bitmap = render_icon({".png", png}, {16})[0];
    CHECK(bitmap.rgba[0] == 255 && bitmap.rgba[1] == 0 && bitmap.rgba[3] == 255);
    auto png_ico = container(png, 1);
    CHECK(render_icon({".ico", png_ico}, {32})[0].rgba[0] == 255);
    std::vector<uint8_t> dib(40 + 16 + 8);
    put32(dib, 0, 40); put32(dib, 4, 2); put32(dib, 8, 4); dib[12] = 1; dib[14] = 32;
    for (size_t i = 40; i < 56; i += 4) { dib[i + 1] = 255; dib[i + 3] = 255; }
    dib[60] = 0x80; // Transparent upper-left pixel in the bottom-up AND mask.
    auto dib_ico = container(dib, 2);
    bitmap = render_icon({".ico", dib_ico}, {16})[0];
    CHECK(bitmap.rgba[3] == 0 && bitmap.rgba[16 * 16 * 4 - 3] == 255 && bitmap.rgba.back() == 255);
    dib[14] = 24; dib.resize(40 + 16 + 8);
    for (size_t row : {size_t(40), size_t(48)}) for (size_t x : {size_t(0), size_t(3)}) { dib[row+x] = 0; dib[row+x+1] = 255; dib[row+x+2] = 0; }
    bitmap = render_icon({".ico", container(dib, 2)}, {16})[0];
    CHECK(bitmap.rgba[3] == 0 && bitmap.rgba[16 * 16 * 4 - 3] == 255 && bitmap.rgba.back() == 255);
    auto corrupt = png_ico; put32(corrupt, 18, 0xfffffff0); rejects({".ico", corrupt});
    std::vector<uint8_t> frames(38); frames[2] = 1; frames[4] = 2;
    for (int i = 0; i < 2; ++i) {
      const int size = i ? 32 : 16; const size_t entry = 6 + i * 16;
      std::vector<uint8_t> frame(40 + size * size * 4 + size * ((size + 31) / 32) * 4);
      put32(frame, 0, 40); put32(frame, 4, size); put32(frame, 8, size * 2); frame[12] = 1; frame[14] = 32;
      for (int pixel = 0; pixel < size * size; ++pixel) { frame[40 + pixel * 4 + (i ? 0 : 2)] = 255; frame[43 + pixel * 4] = 255; }
      frames[entry] = frames[entry + 1] = uint8_t(size); frames[entry + 6] = 32;
      put32(frames, entry + 8, uint32_t(frame.size())); put32(frames, entry + 12, uint32_t(frames.size()));
      const auto offset = frames.size(); frames.resize(offset + frame.size());
      std::copy(frame.begin(), frame.end(), frames.begin() + offset);
    }
    auto multi = render_icon({".ico", frames}, {16, 24, 32});
    CHECK(multi[0].rgba[0] == 255 && multi[0].rgba[2] == 0);
    CHECK(multi[1].rgba[0] == 0 && multi[1].rgba[2] == 255 && multi[2].rgba[2] == 255);
    rejects({".ico", {0, 0, 1, 0, 0, 0}}); rejects({".png", {1, 2, 3}});
    rejects(svg("<svg")); rejects(svg("<svg width='0' height='0'/>"));
    auto root = fs::temp_directory_path() / ("reaweb-icons-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code error; fs::remove_all(root, error); } } cleanup{root};
    auto path = root / fs::u8path(u8"图标.SVG");
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(source.bytes.data()), source.bytes.size());
    auto loaded = load_icon(root, path.filename().u8string());
    CHECK(loaded->format == ".svg" && loaded->bytes == source.bytes);
    CHECK(load_icon(root, path.u8string())->bytes == source.bytes);
    fs::remove(path);
    CHECK(render_icon(*loaded, {128})[0].size == 128);
    CHECK(fs::is_empty(root)); // Rendering writes no image files.
    { std::ofstream file(path, std::ios::binary); file.seekp(4 * 1024 * 1024); file.put(' '); }
    try { load_icon(root, path.u8string()); throw std::runtime_error("Oversize icon accepted"); }
    catch (const Error& error) { CHECK(error.code == "ICON_LIMIT"); }
#ifdef _WIN32
    auto window = CreateWindowExW(0, L"STATIC", L"Icon test", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(window);
    {
      WinIcon native;
      native.set(window, images);
      auto first = reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_SMALL, 0)); CHECK(first);
      ICONINFO info{}; CHECK(GetIconInfo(first, &info));
      BITMAP description{}; CHECK(GetObjectW(info.hbmColor, sizeof(description), &description));
      CHECK(description.bmWidth == 16 && description.bmHeight == 16);
      DeleteObject(info.hbmColor); DeleteObject(info.hbmMask);
      SendMessageW(window, WM_SETICON, ICON_SMALL, 0); SendMessageW(window, WM_SETICON, ICON_BIG, 0);
      native.refresh(window);
      CHECK(reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_SMALL, 0)) == first);
      native.set_visible(window, false);
      CHECK(!SendMessageW(window, WM_GETICON, ICON_SMALL, 0) && !SendMessageW(window, WM_GETICON, ICON_BIG, 0));
      CHECK(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_DLGMODALFRAME);
      CHECK((GetWindowLongPtrW(window, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW);
      native.set(window, images);
      CHECK(!SendMessageW(window, WM_GETICON, ICON_SMALL, 0));
      DestroyWindow(window);
      window = CreateWindowExW(0, L"STATIC", L"Recreated icon test", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      CHECK(window); native.refresh(window);
      CHECK(!SendMessageW(window, WM_GETICON, ICON_SMALL, 0) && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_DLGMODALFRAME));
      native.set_visible(window, true);
      CHECK(SendMessageW(window, WM_GETICON, ICON_SMALL, 0) && !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_DLGMODALFRAME));
      for (int i = 0; i < 20; ++i) native.set(window, images);
      auto latest = reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_BIG, 0)); CHECK(latest);
      CHECK(GetIconInfo(latest, &info));
      CHECK(GetObjectW(info.hbmColor, sizeof(description), &description)); CHECK(description.bmWidth == 64);
      DeleteObject(info.hbmColor); DeleteObject(info.hbmMask);
      native.clear(window);
      CHECK(!SendMessageW(window, WM_GETICON, ICON_SMALL, 0) && !SendMessageW(window, WM_GETICON, ICON_BIG, 0));
      native.set(window, images);
      DestroyWindow(window);
    }
#endif
    std::cout << "Window icon formats, transparency, paths and memory rendering passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
