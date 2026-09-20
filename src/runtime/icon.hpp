#pragma once
#include "core/core.hpp"
#include <memory>

namespace reaweb {
struct IconSource { std::string format; std::vector<uint8_t> bytes; };
struct IconBitmap { int size = 0; std::vector<uint8_t> rgba; };
std::shared_ptr<const IconSource> load_icon(const fs::path& root, const Json& path);
std::shared_ptr<const IconSource> icon_from_page(const Json& value);
std::vector<IconBitmap> render_icon(const IconSource& source, const std::vector<int>& sizes);
}
