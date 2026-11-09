#pragma once
#include <string>
#include <vector>

namespace reaweb {
// Empty on malformed or unsupported images. Excludes build metadata, not code or layout.
std::string video_module_fingerprint(std::vector<unsigned char> bytes);
}
