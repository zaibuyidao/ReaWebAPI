#pragma once
#include "core/core.hpp"
namespace reaweb {
fs::path existing_local_path(const fs::path& base, const Json& input);
bool is_file_method(const std::string& name);
Json file_call(const fs::path& base, const std::string& method, const Json& args);
}
