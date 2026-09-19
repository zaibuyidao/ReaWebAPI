#include "runtime/services.hpp"

namespace reaweb {
std::string runtime_platform() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

std::string runtime_architecture() {
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
  return "x64";
#elif defined(_M_IX86) || defined(__i386__)
  return "x86";
#elif defined(__arm__)
  return "arm";
#else
  return "unknown";
#endif
}
}
