#pragma once
#include "core.hpp"
namespace reaweb {
bool is_file_method(const std::string& name);
Json file_call(const fs::path& base, const std::string& method, const Json& args);
}
