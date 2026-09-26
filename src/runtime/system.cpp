#include "runtime/services.hpp"
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace reaweb {
Json system_diagnostics() {
  double seconds = 0;
#ifdef _WIN32
  FILETIME created{}, exited{}, kernel{}, user{};
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    seconds = (double((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
               double((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime)) / 10000000;
#else
  struct rusage usage{};
  if (!getrusage(RUSAGE_SELF, &usage)) seconds = usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000.0;
#endif
  return {{"logicalProcessors", std::thread::hardware_concurrency()}, {"processCpuSeconds", seconds}};
}
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
