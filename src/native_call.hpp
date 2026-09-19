#pragma once
#include "native.hpp"
#include <reaper_plugin.h>
#include <type_traits>
#include <vector>

class ProjectMarker;
class AudioAccessor;
class joystick_device;

namespace reaweb {
class NativeFrame {
public:
  NativeFrame(NativeContext& context, const NativeEntry& entry, const Json& args);
  ~NativeFrame();
  NativeFrame(const NativeFrame&) = delete;
  NativeFrame& operator=(const NativeFrame&) = delete;
  template<class T> T argument(size_t i) {
    auto& c = cells_[i];
    if constexpr (std::is_pointer_v<T>) return static_cast<T>(c.pointer);
    else if constexpr (std::is_same_v<T, bool>) return c.boolean;
    else if constexpr (std::is_same_v<T, double>) return c.number;
    else if constexpr (std::is_same_v<T, size_t>) return c.size;
    else if constexpr (std::is_same_v<T, unsigned int>) return c.uint;
    else return c.integer;
  }
  template<class T> void capture(T value) {
    if constexpr (std::is_same_v<T, const char*> || std::is_same_v<T, char*>)
      results_[0] = value ? Json(std::string(value)) : Json(nullptr);
    else if constexpr (std::is_same_v<T, GUID*>) results_[0] = value ? Json(guid_string(*value)) : Json(nullptr);
    else if constexpr (std::is_pointer_v<T>) results_[0] = export_pointer(value, entry_.returns);
    else results_[0] = value;
  }
  Json finish();
  bool grow_read_buffer();
private:
  struct Cell {
    int integer = 0, length = 0;
    unsigned int uint = 0;
    size_t size = 0;
    bool boolean = false;
    double number = 0;
    void* pointer = nullptr;
    void* object = nullptr;
    char* buffer = nullptr;
    const char* text = nullptr;
    std::vector<char> storage;
    std::vector<double> samples;
    GUID guid{};
    RECT rect{};
  };
  NativeContext& context_;
  const NativeEntry& entry_;
  const Json& args_;
  std::vector<Cell> cells_;
  Json results_;
  std::vector<int> realloc_tokens_;
  std::string owner_;
  void* project_ = nullptr;
  Json export_pointer(void*, std::string type);
  void cleanup() noexcept;
  static std::string guid_string(const GUID& guid);
};
}
