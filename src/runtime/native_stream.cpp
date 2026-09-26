#include "runtime/native_stream.hpp"
#include "runtime/stream_transport.hpp"
#include <cmath>
#include <cstring>
#include <random>
#include <regex>

namespace reaweb {
StreamBuffer::StreamBuffer(int type, const ReaWeb_StreamDesc& desc) : kind(type), descriptor(desc), slots_(new Slot[desc.capacity]) {
  for (unsigned i = 0; i < desc.capacity; ++i) slots_[i].data.reset(new unsigned char[desc.max_bytes]);
}
int StreamBuffer::publish(const void* data, uint32_t bytes, uint64_t sequence, double timestamp) noexcept {
  if (closed.load(std::memory_order_acquire)) return REAWEB_STREAM_CLOSED;
  if (!data || !bytes || bytes > descriptor.max_bytes || !std::isfinite(timestamp)) return REAWEB_INVALID_ARGUMENT;
  if (kind == REAWEB_FRAME && bytes != descriptor.stride * descriptor.height) return REAWEB_INVALID_ARGUMENT;
  if (descriptor.format == REAWEB_FLOAT32 && bytes % (sizeof(float) *
      ((kind == REAWEB_AUDIO || kind == REAWEB_SPECTRUM || kind == REAWEB_WAVEFORM) ? std::max(1u, descriptor.channels) : 1u))) return REAWEB_INVALID_ARGUMENT;
  if (writing_.test_and_set(std::memory_order_acquire)) return REAWEB_PRODUCER_BUSY;
  struct Unlock { std::atomic_flag& flag; ~Unlock() { flag.clear(std::memory_order_release); } } unlock{writing_};
  auto& slot = slots_[write_];
  unsigned expected = 0;
  if (!slot.state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
    if (kind == REAWEB_AUDIO || kind == REAWEB_MIDI || expected != 2 ||
        !slot.state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
      dropped.fetch_add(1, std::memory_order_relaxed); return REAWEB_BUFFER_FULL;
    }
    dropped.fetch_add(1, std::memory_order_relaxed);
  }
  std::memcpy(slot.data.get(), data, bytes);
  slot.bytes = bytes; slot.sequence = sequence; slot.timestamp = timestamp; slot.order = ++order_;
  slot.state.store(2, std::memory_order_release);
  write_ = (write_ + 1) % descriptor.capacity;
  published.fetch_add(1, std::memory_order_relaxed);
  return REAWEB_OK;
}
bool StreamBuffer::consume(Packet& packet) {
  if (kind == REAWEB_AUDIO || kind == REAWEB_MIDI) {
    auto& slot = slots_[read_]; unsigned expected = 2;
    if (!slot.state.compare_exchange_strong(expected, 3, std::memory_order_acquire)) return false;
    packet = {slot.sequence, slot.timestamp, dropped.load(), {slot.data.get(), slot.data.get() + slot.bytes}};
    slot.state.store(0, std::memory_order_release); read_ = (read_ + 1) % descriptor.capacity; return true;
  }
  // A producer can refill an already-scanned slot before this scan reaches a
  // newer slot. Never deliver that older packet during the next scan.
  uint64_t newest = consumed_order_; bool found = false;
  for (unsigned i = 0; i < descriptor.capacity; ++i) {
    auto& slot = slots_[i]; unsigned expected = 2;
    if (!slot.state.compare_exchange_strong(expected, 3, std::memory_order_acquire)) continue;
    if (slot.order > newest) {
      newest = slot.order; found = true;
      packet = {slot.sequence, slot.timestamp, dropped.load(), {slot.data.get(), slot.data.get() + slot.bytes}};
    }
    slot.state.store(0, std::memory_order_release);
  }
  consumed_order_ = newest;
  return found;
}
StreamHub::StreamHub() = default;
void StreamHub::reap() const {
  for (auto it = retired_.begin(); it != retired_.end();) {
    if (it->first.expired()) { allocated_ -= it->second; it = retired_.erase(it); }
    else ++it;
  }
}
StreamHub::~StreamHub() {
  transport_.reset();
  for (auto& slot : slots_) {
    slot.buffer.store(nullptr, std::memory_order_release);
    while (slot.readers.load(std::memory_order_acquire)) std::this_thread::yield();
  }
}
const char* StreamHub::code(int status) {
  switch (status) {
    case REAWEB_STREAM_NOT_FOUND: return "STREAM_NOT_FOUND";
    case REAWEB_STREAM_CLOSED: return "STREAM_CLOSED";
    case REAWEB_UNSUPPORTED_FORMAT: return "UNSUPPORTED_FORMAT";
    case REAWEB_BUFFER_FULL: return "BUFFER_FULL";
    case REAWEB_PRODUCER_BUSY: return "PRODUCER_BUSY";
    case REAWEB_EXTENSION_UNLOADED: return "EXTENSION_UNLOADED";
    case REAWEB_MAIN_THREAD_REQUIRED: return "WRONG_THREAD";
    case REAWEB_QUEUE_LIMIT: return "QUEUE_LIMIT";
    case REAWEB_TIMEOUT: return "TIMEOUT";
    case REAWEB_SERVICE_ERROR: return "NATIVE_ERROR";
    case REAWEB_SERVICE_EXISTS: return "STREAM_EXISTS";
    default: return "INVALID_ARGUMENT";
  }
}
int StreamHub::create(int kind, const char* name, const ReaWeb_StreamDesc* input, uint64_t* handle) {
  if (std::this_thread::get_id() != main_) return REAWEB_MAIN_THREAD_REQUIRED;
  if (!name || !input || !handle || input->size != sizeof(*input) || input->abi_version != REAWEB_STREAM_ABI ||
      kind < REAWEB_FRAME || kind > REAWEB_MIDI || !std::regex_match(name, std::regex("[A-Za-z0-9_.-]{1,128}"))) return REAWEB_INVALID_ARGUMENT;
  auto desc = *input;
  if (!desc.max_bytes || desc.max_bytes > 16 * 1024 * 1024 || desc.capacity < 2 || desc.capacity > 64 ||
      desc.channels > 32 || !std::isfinite(desc.update_rate) || desc.update_rate < 0 || desc.update_rate > 1000 ||
      (desc.source && std::strlen(desc.source) > 1024)) return REAWEB_INVALID_ARGUMENT;
  if (kind == REAWEB_FRAME) {
    if (desc.format != REAWEB_PIXEL_RGBA8 && desc.format != REAWEB_PIXEL_BGRA8) return REAWEB_UNSUPPORTED_FORMAT;
    if (!desc.width || !desc.height || desc.width > 8192 || desc.height > 8192 || desc.stride < desc.width * 4 ||
        uint64_t(desc.stride) * desc.height != desc.max_bytes) return REAWEB_INVALID_ARGUMENT;
  } else if (kind == REAWEB_BINARY || kind == REAWEB_MIDI) {
    if (desc.format != REAWEB_BYTES) return REAWEB_UNSUPPORTED_FORMAT;
  } else {
    if (desc.format != REAWEB_FLOAT32) return REAWEB_UNSUPPORTED_FORMAT;
    if (!desc.channels || !desc.sample_rate || desc.sample_rate > 768000) return REAWEB_INVALID_ARGUMENT;
    if (kind == REAWEB_AUDIO && (!desc.block_frames || uint64_t(desc.block_frames) * desc.channels * 4 != desc.max_bytes)) return REAWEB_INVALID_ARGUMENT;
    if (kind == REAWEB_SPECTRUM && (desc.fft_size < 32 || desc.fft_size > 32768 || (desc.fft_size & (desc.fft_size - 1)))) return REAWEB_INVALID_ARGUMENT;
  }
  const auto allocation = size_t(desc.capacity) * desc.max_bytes;
  std::lock_guard<std::mutex> lock(mutex_);
  reap();
  if (allocation > 256 * 1024 * 1024 - allocated_) return REAWEB_QUEUE_LIMIT;
  Slot* free = nullptr; unsigned index = 0;
  for (unsigned i = 0; i < slots_.size(); ++i) {
    auto& slot = slots_[i];
    if (slot.owned && slot.name == name) return REAWEB_SERVICE_EXISTS;
    if (!slot.owned && !free) { free = &slot; index = i; }
  }
  if (!free) return REAWEB_QUEUE_LIMIT;
  free->source = desc.source ? desc.source : ""; desc.source = nullptr;
  free->owned = std::make_shared<StreamBuffer>(kind, desc);
  free->name = name; free->owner = desc.owner; free->allocation = allocation; allocated_ += allocation;
  *handle = (++serial_ << 8) | index;
  free->handle.store(*handle, std::memory_order_release);
  free->buffer.store(free->owned.get(), std::memory_order_release);
  return REAWEB_OK;
}
int StreamHub::publish(int kind, uint64_t handle, const void* data, uint32_t bytes, uint64_t sequence, double timestamp) noexcept {
  const unsigned index = handle & 255;
  if (!handle || index >= slots_.size()) return REAWEB_STREAM_NOT_FOUND;
  auto& slot = slots_[index]; slot.readers.fetch_add(1, std::memory_order_seq_cst);
  auto* buffer = slot.buffer.load(std::memory_order_seq_cst);
  int status = REAWEB_STREAM_CLOSED;
  if (buffer && slot.handle.load(std::memory_order_acquire) == handle)
    status = buffer->kind == kind ? buffer->publish(data, bytes, sequence, timestamp) : REAWEB_UNSUPPORTED_FORMAT;
  slot.readers.fetch_sub(1, std::memory_order_release);
  return status;
}
int StreamHub::close(uint64_t handle, int reason) {
  if (std::this_thread::get_id() != main_) return REAWEB_MAIN_THREAD_REQUIRED;
  const unsigned index = handle & 255;
  if (!handle || index >= slots_.size()) return REAWEB_STREAM_NOT_FOUND;
  std::lock_guard<std::mutex> lock(mutex_); auto& slot = slots_[index];
  if (slot.handle.load() != handle) return REAWEB_STREAM_CLOSED;
  if (!slot.owned) return REAWEB_OK;
  slot.owned->closed.store(reason, std::memory_order_release);
  slot.buffer.store(nullptr, std::memory_order_seq_cst);
  while (slot.readers.load(std::memory_order_seq_cst)) std::this_thread::yield();
  retired_.emplace_back(slot.owned, slot.allocation); slot.owned.reset(); reap();
  return REAWEB_OK;
}
void StreamHub::close_owner(uint64_t owner) {
  for (auto& slot : slots_) if (slot.owned && slot.owner == owner) close(slot.handle.load(), REAWEB_EXTENSION_UNLOADED);
}
Json StreamHub::attach(const std::string& name, int window, uint64_t generation, const std::string& origin) {
  if (!transport_) transport_ = std::make_unique<StreamTransport>(*this);
  std::lock_guard<std::mutex> lock(mutex_);
  if (tickets_.size() >= 64) throw Error("QUEUE_LIMIT", "At most 64 stream consumers may be attached");
  for (const auto& slot : slots_) if (slot.owned && slot.name == name) {
    std::random_device random; std::string token;
    for (int i = 0; i < 8; ++i) { char text[9]; std::snprintf(text, sizeof(text), "%08x", random()); token += text; }
    tickets_.emplace(token, Ticket{window, generation, origin, slot.owned});
    const auto& d = slot.owned->descriptor;
    static const char* kinds[] = {"", "frame", "audio", "spectrum", "meter", "waveform", "binary", "midi"};
    static const char* formats[] = {"", "rgba8", "bgra8", "float32", "bytes"};
    return {{"token", token}, {"url", transport_->url() + "/" + token}, {"name", name}, {"kind", kinds[slot.owned->kind]},
      {"format", formats[d.format]}, {"capacity", d.capacity}, {"maxBytes", d.max_bytes}, {"width", d.width}, {"height", d.height},
      {"stride", d.stride}, {"channels", d.channels}, {"sampleRate", d.sample_rate}, {"blockFrames", d.block_frames},
      {"fftSize", d.fft_size}, {"binHz", d.fft_size ? double(d.sample_rate) / d.fft_size : 0}, {"updateRate", d.update_rate}, {"source", slot.source},
      {"policy", slot.owned->kind == REAWEB_AUDIO || slot.owned->kind == REAWEB_MIDI ? "drop-new" : "latest"}};
  }
  throw Error("STREAM_NOT_FOUND", "Native stream not registered: " + name);
}
void StreamHub::detach(const std::string& token, int window) {
  std::lock_guard<std::mutex> lock(mutex_); auto it = tickets_.find(token);
  if (it != tickets_.end() && it->second.window == window) tickets_.erase(it);
}
void StreamHub::detach_window(int window) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = tickets_.begin(); it != tickets_.end();) if (it->second.window == window) it = tickets_.erase(it); else ++it;
}
Json StreamHub::info() const {
  std::lock_guard<std::mutex> lock(mutex_); Json streams = Json::array();
  reap();
  for (const auto& slot : slots_) if (slot.owned) streams.push_back({{"name", slot.name}, {"published", slot.owned->published.load()}, {"dropped", slot.owned->dropped.load()}});
  return {{"allocatedBytes", allocated_}, {"consumers", tickets_.size()}, {"streams", streams}};
}
size_t StreamHub::consumers(uint64_t handle) const {
  std::lock_guard<std::mutex> lock(mutex_); const auto index = handle & 255;
  if (index >= slots_.size() || slots_[index].handle.load() != handle || !slots_[index].owned) return 0;
  size_t count = 0; for (const auto& ticket : tickets_) if (ticket.second.buffer == slots_[index].owned) ++count;
  return count;
}
}
