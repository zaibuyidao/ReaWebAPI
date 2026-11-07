#pragma once
#include "core/core.hpp"
#include "public/reaweb_stream.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <optional>

namespace reaweb {
class JsfxTransfer;
class StreamBuffer {
public:
  struct Timing {
    uint64_t clock_epoch = 0, first_sample = 0, source_dropped = 0;
    double position = 0;
    unsigned play_state = 0;
    bool discontinuity = false;
  };
  struct Packet { uint64_t sequence; double timestamp; uint64_t dropped; std::vector<unsigned char> data; std::optional<Timing> timing; };
  StreamBuffer(int kind, const ReaWeb_StreamDesc& desc);
  int publish(const void* data, uint32_t bytes, uint64_t sequence, double timestamp, const Timing* timing = nullptr) noexcept;
  bool consume(Packet& packet);
  const int kind;
  const ReaWeb_StreamDesc descriptor;
  std::atomic<int> closed{0};
  std::atomic<uint64_t> published{0}, dropped{0};
  std::atomic<double> frame_ack_ms{-1};
private:
  struct Slot {
    std::atomic<unsigned> state{0};
    uint32_t bytes = 0;
    uint64_t sequence = 0, order = 0;
    double timestamp = 0;
    std::optional<Timing> timing;
    std::unique_ptr<unsigned char[]> data;
  };
  std::unique_ptr<Slot[]> slots_;
  std::atomic_flag writing_ = ATOMIC_FLAG_INIT;
  uint64_t order_ = 0, consumed_order_ = 0;
  unsigned write_ = 0, read_ = 0;
};

class StreamHub {
public:
  StreamHub();
  ~StreamHub();
  int create(int kind, const char* name, const ReaWeb_StreamDesc* desc, uint64_t* handle);
  int create_jsfx(int kind, const char* name, const ReaWeb_StreamDesc* desc, uint64_t* handle,
                  int window, uint64_t generation, const Json& metadata);
  int publish(int kind, uint64_t handle, const void* data, uint32_t bytes, uint64_t sequence, double timestamp,
              const StreamBuffer::Timing* timing = nullptr) noexcept;
  int close(uint64_t handle, int reason = REAWEB_STREAM_CLOSED);
  void close_owner(uint64_t owner);
  Json attach(const std::string& name, int window, uint64_t generation, const std::string& origin);
  Json attach_external(const std::string& name, uint64_t session);
  Json attach_jsfx_resource(const std::shared_ptr<JsfxTransfer>& transfer,int window,uint64_t generation,const std::string& origin);
  void detach_external(uint64_t session, const std::string& token = {});
  void detach(const std::string& token, int window);
  void detach_window(int window);
  void detach_stream_window(uint64_t handle, int window);
  Json info() const;
  size_t consumers(uint64_t handle) const;
  double frame_ack_ms(uint64_t handle) const;
  static const char* code(int status);
private:
  friend class StreamTransport;
  struct Slot {
    std::atomic<StreamBuffer*> buffer{nullptr};
    std::atomic<unsigned> readers{0};
    std::atomic<uint64_t> handle{0};
    std::shared_ptr<StreamBuffer> owned;
    std::string name, source;
    uint64_t owner = 0;
    int protected_window = 0;
    uint64_t protected_generation = 0;
    Json metadata;
    size_t allocation = 0;
  };
  struct Ticket {
    int window;
    uint64_t generation;
    std::string origin;
    std::shared_ptr<StreamBuffer> buffer;
    bool connected = false;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    uint64_t external = 0;
    std::shared_ptr<JsfxTransfer> reliable{};
  };
  static constexpr unsigned slot_count = 128;
  std::array<Slot, slot_count> slots_;
  std::map<std::string, Ticket> tickets_;
  mutable std::mutex mutex_;
  mutable size_t allocated_ = 0;
  mutable std::vector<std::pair<std::weak_ptr<StreamBuffer>, size_t>> retired_;
  void reap() const;
  uint64_t serial_ = 0;
  std::thread::id main_ = std::this_thread::get_id();
  std::unique_ptr<class StreamTransport> transport_;
  Json attach_consumer(const std::string& name, int window, uint64_t generation, const std::string& origin, uint64_t external);
};
}
