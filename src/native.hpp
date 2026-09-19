#pragma once
#include "core.hpp"
#include <memory>
#include <vector>

namespace reaweb {
class NativeFrame;
enum class Kind { Int, UInt, Size, Bool, Double, IntPtr, UIntPtr, BoolPtr, DoublePtr,
  String, Buffer, StringOut, Length, LengthPtr, Guid, Rect, Array, Handle, HandleOut };
struct NativeParam {
  Kind kind;
  int input, output, link, flags;
  const char* handle;
};
struct NativeEntry {
  const char* name;
  const char* returns;
  int min_args, max_args, return_count;
  std::vector<NativeParam> parameters;
  void (*invoke)(NativeFrame&, void*);
};
const std::vector<NativeEntry>& native_entries();

// One context per WebView document. Pointer values never cross the bridge.
class NativeContext {
public:
  NativeContext(Host& host, std::string session);
  ~NativeContext();
  Json invoke(const NativeEntry& entry, const Json& args);
  void validate(const NativeEntry& entry, const Json& args);
  void reset();
  void* resolve(const char* name) const;
  Json capabilities() const;
  void* pointer(const Json& value, const std::string& type, bool validate = true);
  Json handle(void* pointer, const std::string& type, void* project = nullptr,
              const std::string& owner = {}, const std::string& destroy = {});
  void invalidate(void* pointer);
  void transfer(void* pointer, const std::string& owner);
  void* current_project() const;
  std::string identity(void* pointer, const std::string& type) const;
  std::string parent_token(const Json& args) const;
  size_t buffer_size() const;
  size_t set_buffer_size(size_t size);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
