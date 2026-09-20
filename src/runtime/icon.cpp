#include "runtime/icon.hpp"
#include "runtime/fs.hpp"
#include <lunasvg.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 4096
#include <plutovg-stb-image.h>

namespace reaweb {
namespace {
uint32_t u32(const uint8_t* p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
unsigned u16(const uint8_t* p) { return p[0] | unsigned(p[1]) << 8; }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
[[noreturn]] void invalid() { throw Error("ICON_INVALID", "Invalid or unsupported icon image"); }
struct Raster { int width = 0, height = 0; std::vector<uint8_t> rgba; };
bool png(const uint8_t* data, size_t size) {
  static const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  return size >= sizeof(signature) && !std::memcmp(data, signature, sizeof(signature));
}
Raster decode(const uint8_t* data, size_t size) {
  int width = 0, height = 0, channels = 0;
  if (!stbi_info_from_memory(data, static_cast<int>(size), &width, &height, &channels) ||
      width <= 0 || height <= 0 || width > 4096 || height > 4096) invalid();
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
    stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, 4), stbi_image_free);
  if (!pixels) invalid();
  return {width, height, {pixels.get(), pixels.get() + size_t(width) * height * 4}};
}
Raster ico(const std::vector<uint8_t>& bytes, int target) {
  if (bytes.size() < 6 || u16(bytes.data()) || u16(bytes.data() + 2) != 1) invalid();
  const auto count = u16(bytes.data() + 4);
  if (!count || count > 256 || bytes.size() < 6 + size_t(count) * 16) invalid();
  const uint8_t* selected = nullptr;
  int score = 100000;
  for (unsigned i = 0; i < count; ++i) {
    const auto entry = bytes.data() + 6 + i * 16;
    const auto length = u32(entry + 8), offset = u32(entry + 12);
    if (offset < 6 + count * 16 || offset > bytes.size() || length > bytes.size() - offset || !length) invalid();
    const int width = entry[0] ? entry[0] : 256, height = entry[1] ? entry[1] : 256;
    const int dimension = std::max(width, height);
    const int distance = std::abs(dimension - target) * 64 + (dimension < target ? 32 : 0) - int(std::min(32u, u16(entry + 6)));
    if (distance < score) { selected = entry; score = distance; }
  }
  const auto data = bytes.data() + u32(selected + 12);
  const auto length = u32(selected + 8);
  if (png(data, length)) return decode(data, length);
  if (length < 40) invalid();
  const auto header = u32(data), width = u32(data + 4), twice_height = u32(data + 8), compression = u32(data + 16);
  const auto bits = u16(data + 14);
  if ((header != 40 && header != 108 && header != 124) || length < header || !width || width > 256 ||
      !twice_height || twice_height > 512 || twice_height % 2 || u16(data + 12) != 1 ||
      (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
      (compression != 0 && compression != 3) || (compression == 3 && bits != 16 && bits != 32)) invalid();
  const auto height = twice_height / 2;
  const auto colors = bits <= 8 ? (u32(data + 32) ? u32(data + 32) : 1u << bits) : 0;
  if (colors > 256) invalid();
  const size_t offset = header + colors * 4 + (header == 40 && compression == 3 ? 12 : 0);
  const size_t stride = ((width * bits + 31) / 32) * 4, mask_stride = ((width + 31) / 32) * 4;
  if (offset > length || stride * height > length - offset) invalid();
  std::vector<uint8_t> bmp(14 + offset + stride * height);
  bmp[0] = 'B'; bmp[1] = 'M'; put32(bmp.data() + 2, static_cast<uint32_t>(bmp.size()));
  put32(bmp.data() + 10, static_cast<uint32_t>(14 + offset));
  std::memcpy(bmp.data() + 14, data, bmp.size() - 14);
  put32(bmp.data() + 22, height);
  auto result = decode(bmp.data(), bmp.size());
  const size_t mask = offset + stride * height;
  if (length - mask >= mask_stride * height) {
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
      if (data[mask + (height - 1 - y) * mask_stride + x / 8] & (0x80 >> (x % 8)))
        result.rgba[(size_t(y) * width + x) * 4 + 3] = 0;
  } else if (bits != 32) invalid();
  return result;
}
IconBitmap resize(const Raster& raster, int size) {
  IconBitmap result{size, std::vector<uint8_t>(size_t(size) * size * 4)};
  const double scale = double(size) / std::max(raster.width, raster.height);
  const int width = std::max(1, int(std::lround(raster.width * scale))), height = std::max(1, int(std::lround(raster.height * scale)));
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    const double sx = std::clamp((x + 0.5) / scale - 0.5, 0.0, double(raster.width - 1));
    const double sy = std::clamp((y + 0.5) / scale - 0.5, 0.0, double(raster.height - 1));
    const int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, raster.width - 1), y1 = std::min(y0 + 1, raster.height - 1);
    const double fx = sx - x0, fy = sy - y0;
    const size_t indexes[] = {size_t(y0) * raster.width + x0, size_t(y0) * raster.width + x1,
      size_t(y1) * raster.width + x0, size_t(y1) * raster.width + x1};
    const double weights[] = {(1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy};
    double alpha = 0, colors[3]{};
    for (int i = 0; i < 4; ++i) {
      const auto pixel = raster.rgba.data() + indexes[i] * 4;
      const double a = pixel[3] * weights[i]; alpha += a;
      for (int c = 0; c < 3; ++c) colors[c] += pixel[c] * a;
    }
    auto pixel = result.rgba.data() + (size_t(y + (size-height)/2) * size + x + (size-width)/2) * 4;
    pixel[3] = uint8_t(std::lround(alpha));
    if (alpha > 0) for (int c = 0; c < 3; ++c) pixel[c] = uint8_t(std::lround(colors[c] / alpha));
  }
  return result;
}
}
std::shared_ptr<const IconSource> load_icon(const fs::path& root, const Json& input) {
  auto path = existing_local_path(root, input);
  if (!fs::is_regular_file(path)) throw Error("ICON_INVALID", "Icon path must refer to a regular file");
  auto format = path.extension().u8string();
  std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (format != ".png" && format != ".ico" && format != ".svg") throw Error("ICON_FORMAT", "Window icons must be PNG, ICO or SVG");
  std::ifstream file(path, std::ios::binary);
  auto source = std::make_shared<IconSource>(); source->format = format;
  const auto length = fs::file_size(path);
  if (!length || length > 4 * 1024 * 1024) throw Error("ICON_LIMIT", "Icon files must contain 1 byte to 4 MiB");
  source->bytes.resize(static_cast<size_t>(length));
  if (!file.read(reinterpret_cast<char*>(source->bytes.data()), static_cast<std::streamsize>(length)))
    throw Error("FILE_IO", "Cannot read icon file");
  return source;
}
std::vector<IconBitmap> render_icon(const IconSource& source, const std::vector<int>& sizes) {
  if (sizes.empty() || sizes.size() > 8) invalid();
  std::unique_ptr<lunasvg::Document> svg;
  Raster raster;
  if (source.format == ".svg") {
    svg = lunasvg::Document::loadFromData(reinterpret_cast<const char*>(source.bytes.data()), source.bytes.size());
    if (!svg || !std::isfinite(svg->width()) || !std::isfinite(svg->height()) || svg->width() <= 0 || svg->height() <= 0) invalid();
  } else if (source.format == ".png") {
    if (!png(source.bytes.data(), source.bytes.size())) invalid();
    raster = decode(source.bytes.data(), source.bytes.size());
  } else if (source.format != ".ico") invalid();
  std::vector<IconBitmap> result;
  for (int size : sizes) {
    if (size < 8 || size > 256) throw Error("ICON_LIMIT", "Icon raster dimensions must be 8..256 pixels");
    if (svg) {
      const auto scale = float(size) / std::max(svg->width(), svg->height());
      if (!std::isfinite(scale) || scale <= 0) invalid();
      lunasvg::Bitmap bitmap(size, size);
      if (bitmap.isNull()) invalid();
      bitmap.clear(0);
      svg->render(bitmap, lunasvg::Matrix(scale, 0, 0, scale, (size-svg->width()*scale)/2, (size-svg->height()*scale)/2));
      bitmap.convertToRGBA();
      result.push_back({size, {bitmap.data(), bitmap.data() + size_t(size) * size * 4}});
    } else result.push_back(resize(source.format == ".ico" ? ico(source.bytes, size) : raster, size));
  }
  return result;
}
std::shared_ptr<const IconSource> icon_from_page(const Json& value) {
  if (!value.is_object() || !value.contains("format") || !value["format"].is_string() || !value.contains("bytes"))
    throw Error("INVALID_ARGUMENT", "Expected favicon format and bytes");
  const auto format = value["format"].get<std::string>();
  if (format != ".png" && format != ".ico" && format != ".svg") throw Error("ICON_FORMAT", "Favicons must be PNG, ICO or SVG");
  const auto& bytes = value["bytes"];
  if (!bytes.is_object() || !bytes.contains("__reawebBytes") || !bytes["__reawebBytes"].is_string())
    throw Error("INVALID_ARGUMENT", "Expected encoded favicon bytes");
  if (bytes["__reawebBytes"].get_ref<const std::string&>().size() > ((4 * 1024 * 1024 + 2) / 3) * 4)
    throw Error("ICON_LIMIT", "Icon files must contain 1 byte to 4 MiB");
  const auto decoded = decode_binary(bytes);
  if (decoded.empty() || decoded.size() > 4 * 1024 * 1024) throw Error("ICON_LIMIT", "Icon files must contain 1 byte to 4 MiB");
  return std::make_shared<IconSource>(IconSource{format, {decoded.begin(), decoded.end()}});
}
}
