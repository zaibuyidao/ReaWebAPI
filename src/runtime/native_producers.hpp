#pragma once
#include "runtime/native_stream.hpp"
namespace reaweb {
class NativeProducers {
public:
  NativeProducers(const Host& host, StreamHub& streams);
  ~NativeProducers();
  std::string audio(const std::string& kind, const Json& options, int window);
  std::string midi(int device, int window);
  void attached(const std::string& name);
  void tick();
  void close_window(int window);
  /* Hardware hook only. Copies bounded stereo blocks into preallocated rings. */
  void capture(bool output, int frames, double rate, int channels, double* (*get)(bool, int)) noexcept;
  Json devices() const;
  Json diagnostics() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
