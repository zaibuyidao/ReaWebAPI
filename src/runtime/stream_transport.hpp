#pragma once
#include <memory>
#include <string>
namespace reaweb {
class StreamHub;
class StreamTransport {
public:
  explicit StreamTransport(StreamHub& hub);
  ~StreamTransport();
  std::string url() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
