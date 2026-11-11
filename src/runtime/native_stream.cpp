#include "runtime/native_stream.hpp"
#include "runtime/stream_transport.hpp"
#include "runtime/jsfx_transfer.hpp"
#include "runtime/external_settings.hpp"
#include <cmath>
#include <cstring>
#include <random>
#include <regex>

namespace reaweb {
StreamBuffer::StreamBuffer(int type, const ReaWeb_StreamDesc& desc, bool video) : kind(type), descriptor(desc), project_video(video), slots_(new Slot[desc.capacity]) {
  for (unsigned i = 0; i < desc.capacity; ++i) {
    if (project_video) slots_[i].video_data.resize(desc.max_bytes);
    else slots_[i].data.reset(new unsigned char[desc.max_bytes]);
  }
}
int StreamBuffer::publish(const void* data, uint32_t bytes, uint64_t sequence, double timestamp, const Timing* timing, bool discontinuity,
                          std::vector<unsigned char>* owned_video) noexcept {
  if (closed.load(std::memory_order_acquire)) return REAWEB_STREAM_CLOSED;
  if (!data || !bytes || bytes > descriptor.max_bytes || !std::isfinite(timestamp)) return REAWEB_INVALID_ARGUMENT;
  if (owned_video && (!project_video || owned_video->data() != data || owned_video->size() != descriptor.max_bytes)) return REAWEB_INVALID_ARGUMENT;
  if (kind == REAWEB_FRAME && bytes != (descriptor.format == REAWEB_PIXEL_I420 ? descriptor.max_bytes : descriptor.stride * descriptor.height)) return REAWEB_INVALID_ARGUMENT;
  if (descriptor.format == REAWEB_FLOAT32 && bytes % (sizeof(float) *
      ((kind == REAWEB_AUDIO || kind == REAWEB_SPECTRUM || kind == REAWEB_WAVEFORM) ? std::max(1u, descriptor.channels) : 1u))) return REAWEB_INVALID_ARGUMENT;
  if (writing_.test_and_set(std::memory_order_acquire)) return REAWEB_PRODUCER_BUSY;
  struct Unlock { std::atomic_flag& flag; ~Unlock() { flag.clear(std::memory_order_release); } } unlock{writing_};
  bool claimed = false;
  if (project_video) {
    for (unsigned i = 0; i < descriptor.capacity; ++i) {
      const unsigned index = (write_ + i) % descriptor.capacity;
      unsigned empty = 0;
      if (slots_[index].state.compare_exchange_strong(empty, 1, std::memory_order_acquire)) {
        write_ = index; claimed = true; break;
      }
    }
  }
  auto& slot = slots_[write_];
  unsigned expected = 0;
  if (!claimed && !slot.state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
    if (kind == REAWEB_AUDIO || kind == REAWEB_MIDI || expected != 2 ||
        !slot.state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
      dropped.fetch_add(1, std::memory_order_relaxed); return REAWEB_BUFFER_FULL;
    }
    dropped.fetch_add(1, std::memory_order_relaxed);
  }
  if (owned_video) slot.video_data.swap(*owned_video);
  else std::memcpy(project_video ? slot.video_data.data() : slot.data.get(), data, bytes);
  slot.bytes = bytes; slot.sequence = sequence; slot.timestamp = timestamp; slot.order = ++order_;
  slot.timing = timing ? std::optional<Timing>(*timing) : std::nullopt;
  slot.discontinuity = project_video && discontinuity;
  slot.state.store(2, std::memory_order_release);
  write_ = (write_ + 1) % descriptor.capacity;
  published.fetch_add(1, project_video ? std::memory_order_release : std::memory_order_relaxed);
  return REAWEB_OK;
}
bool StreamBuffer::consume(Packet& packet) {
  if (project_video) {
    if (packet.data.size() != descriptor.max_bytes) {
      bool ready = false;
      for (unsigned i = 0; i < descriptor.capacity; ++i) ready |= slots_[i].state.load(std::memory_order_acquire) == 2;
      if (!ready) return false;
      // Allocate before claiming a slot so allocation cannot hold up the producer.
      packet.data.resize(descriptor.max_bytes);
    }
    // A frame published into an already-scanned slot must not be overtaken.
    const auto published_before_scan = published.load(std::memory_order_acquire);
    Slot* oldest = nullptr;
    for (unsigned i = 0; i < descriptor.capacity; ++i) {
      auto& slot = slots_[i]; unsigned expected = 2;
      if (!slot.state.compare_exchange_strong(expected, 3, std::memory_order_acquire)) continue;
      if (slot.order > published_before_scan) { slot.state.store(2, std::memory_order_release); continue; }
      if (slot.order <= consumed_order_) { slot.state.store(0, std::memory_order_release); continue; }
      if (!oldest || slot.order < oldest->order) {
        if (oldest) oldest->state.store(2, std::memory_order_release);
        oldest = &slot;
      } else slot.state.store(2, std::memory_order_release);
    }
    if (!oldest) return false;
    packet.sequence = oldest->sequence; packet.timestamp = oldest->timestamp; packet.dropped = dropped.load(); packet.timing = oldest->timing; packet.discontinuity = oldest->discontinuity;
    packet.data.swap(oldest->video_data);
    packet.data.resize(oldest->bytes);
    consumed_order_ = oldest->order;
    oldest->state.store(0, std::memory_order_release);
    return true;
  }
  if (kind == REAWEB_AUDIO || kind == REAWEB_MIDI) {
    auto& slot = slots_[read_]; unsigned expected = 2;
    if (!slot.state.compare_exchange_strong(expected, 3, std::memory_order_acquire)) return false;
    packet = {slot.sequence, slot.timestamp, dropped.load(), {slot.data.get(), slot.data.get() + slot.bytes}, slot.timing, slot.discontinuity};
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
      packet = {slot.sequence, slot.timestamp, dropped.load(), {slot.data.get(), slot.data.get() + slot.bytes}, slot.timing, slot.discontinuity};
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
  for (auto& ticket : tickets_) if (ticket.second.presenter) ticket.second.presenter->invalidate();
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
int StreamHub::create(int kind, const char* name, const ReaWeb_StreamDesc* input, uint64_t* handle, bool project_video) {
  return create(kind, name, input, handle, project_video, nullptr);
}
int StreamHub::create(int kind, const char* name, const ReaWeb_StreamDesc* input, uint64_t* handle, bool project_video,
                      const Json& video_metadata) {
  if (std::this_thread::get_id() != main_) return REAWEB_MAIN_THREAD_REQUIRED;
  if (!name || !input || !handle || input->size != sizeof(*input) || input->abi_version != REAWEB_STREAM_ABI ||
      kind < REAWEB_FRAME || kind > REAWEB_MIDI || !std::regex_match(name, std::regex("[A-Za-z0-9_.-]{1,128}"))) return REAWEB_INVALID_ARGUMENT;
  auto desc = *input;
  if (!desc.max_bytes || desc.max_bytes > ((kind == REAWEB_FRAME && project_video) ? video_max_frame_bytes : 16u * 1024 * 1024) || desc.capacity < 2 || desc.capacity > 64 ||
      desc.channels > 32 || !std::isfinite(desc.update_rate) || desc.update_rate < 0 || desc.update_rate > 1000 ||
      (desc.source && std::strlen(desc.source) > 1024)) return REAWEB_INVALID_ARGUMENT;
  if (kind == REAWEB_FRAME) {
    if (desc.format == REAWEB_PIXEL_I420) {
      if (!project_video || !video_metadata.is_object() || video_metadata.value("colorMatrix", Json()) != "bt709" ||
          video_metadata.value("colorRange", Json()) != "limited" ||
          (video_metadata.value("colorConversion", Json()) != "reaper-neon" &&
           video_metadata.value("colorConversion", Json()) != "reaper-sse")) return REAWEB_UNSUPPORTED_FORMAT;
      if (!desc.width || !desc.height || desc.width > 8192 || desc.height > 8192 || (desc.width & 1) || (desc.height & 1) ||
          desc.stride != desc.width || uint64_t(desc.width) * desc.height * 3 / 2 != desc.max_bytes) return REAWEB_INVALID_ARGUMENT;
    } else {
    if (desc.format != REAWEB_PIXEL_RGBA8 && desc.format != REAWEB_PIXEL_BGRA8) return REAWEB_UNSUPPORTED_FORMAT;
    if (!desc.width || !desc.height || desc.width > 8192 || desc.height > 8192 || desc.stride < desc.width * 4 ||
        uint64_t(desc.stride) * desc.height != desc.max_bytes) return REAWEB_INVALID_ARGUMENT;
    }
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
  free->owned = std::make_shared<StreamBuffer>(kind, desc, kind == REAWEB_FRAME && project_video);
  free->name = name; free->owner = desc.owner; free->allocation = allocation; allocated_ += allocation;
  free->protected_window = 0; free->protected_generation = 0; free->metadata = project_video ? video_metadata : Json();
  *handle = (++serial_ << 8) | index;
  free->handle.store(*handle, std::memory_order_release);
  free->buffer.store(free->owned.get(), std::memory_order_release);
  return REAWEB_OK;
}
int StreamHub::create_jsfx(int kind, const char* name, const ReaWeb_StreamDesc* desc, uint64_t* handle,
                           int window, uint64_t generation, const Json& metadata) {
  if (window <= 0 || !metadata.is_object()) return REAWEB_INVALID_ARGUMENT;
  const auto result = create(kind, name, desc, handle);
  if (result != REAWEB_OK) return result;
  std::lock_guard<std::mutex> lock(mutex_);
  auto& slot = slots_[*handle & 255];
  slot.protected_window = window; slot.protected_generation = generation; slot.metadata = metadata;
  return REAWEB_OK;
}
int StreamHub::publish(int kind, uint64_t handle, const void* data, uint32_t bytes, uint64_t sequence, double timestamp,
                       const StreamBuffer::Timing* timing, bool discontinuity, std::vector<unsigned char>* owned_video) noexcept {
  const unsigned index = handle & 255;
  if (!handle || index >= slots_.size()) return REAWEB_STREAM_NOT_FOUND;
  auto& slot = slots_[index]; slot.readers.fetch_add(1, std::memory_order_seq_cst);
  auto* buffer = slot.buffer.load(std::memory_order_seq_cst);
  int status = REAWEB_STREAM_CLOSED;
  if (buffer && slot.handle.load(std::memory_order_acquire) == handle)
    status = buffer->kind == kind ? buffer->publish(data, bytes, sequence, timestamp, timing, discontinuity, owned_video) : REAWEB_UNSUPPORTED_FORMAT;
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
  for (auto it = tickets_.begin(); it != tickets_.end();) {
    if (it->second.buffer == slot.owned && it->second.presenter) {
      it->second.presenter->invalidate();
      it = tickets_.erase(it);
    } else ++it;
  }
  slot.buffer.store(nullptr, std::memory_order_seq_cst);
  while (slot.readers.load(std::memory_order_seq_cst)) std::this_thread::yield();
  retired_.emplace_back(slot.owned, slot.allocation); slot.owned.reset(); reap();
  return REAWEB_OK;
}
void StreamHub::close_owner(uint64_t owner) {
  for (auto& slot : slots_) if (slot.owned && slot.owner == owner) close(slot.handle.load(), REAWEB_EXTENSION_UNLOADED);
}
Json StreamHub::attach(const std::string& name, int window, uint64_t generation, const std::string& origin, const VideoMemoryFactory& video_memory) {
  return attach_consumer(name, window, generation, origin, 0, video_memory);
}
Json StreamHub::attach_external(const std::string& name, uint64_t session) {
  if (!session) throw Error("INVALID_ARGUMENT", "External consumer must have a session");
  return attach_consumer(name, 0, 0, {}, session);
}
Json StreamHub::attach_video_presenter(const std::string& name, int window, uint64_t generation,
                                       const std::string& origin, const VideoPresenterFactory& factory) {
  if (std::this_thread::get_id() != main_) throw Error("WRONG_THREAD", "Video presentation requires the main thread");
  if (window <= 0 || origin.empty() || !factory) throw Error("INVALID_ARGUMENT", "Video presentation requires an App document");
  auto descriptor = attach_consumer(name, window, generation, origin, 0);
  const auto token = descriptor.at("token").get<std::string>();
  std::shared_ptr<VideoPresenter> presenter;
  try {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = tickets_.find(token);
      if (found == tickets_.end() || !found->second.buffer->project_video)
        throw Error("UNSUPPORTED_FORMAT", "Native presentation requires project video");
    }
    descriptor.erase("url");
    presenter = factory(descriptor);
    if (!presenter || !presenter->source() || presenter->closed())
      throw Error("VIDEO_PRESENTATION_UNAVAILABLE", "Native video presentation is unavailable");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = tickets_.find(token);
      if (found == tickets_.end() || found->second.buffer->closed)
        throw Error("STREAM_CLOSED", "Video source closed while preparing presentation");
      found->second.presenter = presenter;
      found->second.connected = true;
    }
    descriptor["renderer"] = presenter->diagnostics().value("renderer", "native");
    return descriptor;
  } catch (...) {
    if (presenter) presenter->invalidate();
    detach(token, window);
    throw;
  }
}
std::shared_ptr<VideoPresenter> StreamHub::video_presenter(const std::string& token, int window, uint64_t generation) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tickets_.find(token);
  if (found == tickets_.end() || found->second.external || found->second.window != window ||
      found->second.generation != generation || !found->second.presenter || found->second.buffer->closed)
    return {};
  return found->second.presenter;
}
Json StreamHub::video_presenter_diagnostics(const std::string& token, int window, uint64_t generation) const {
  if (auto presenter = video_presenter(token, window, generation)) return presenter->diagnostics();
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = video_failures_.find(token);
  if (found != video_failures_.end() && found->second.window == window && found->second.generation == generation &&
      std::chrono::steady_clock::now() - found->second.created < std::chrono::seconds(30))
    return found->second.diagnostics;
  return {{"closed", true}};
}
Json StreamHub::attach_jsfx_resource(const std::shared_ptr<JsfxTransfer>& transfer,int window,uint64_t generation,const std::string& origin) {
  if(std::this_thread::get_id()!=main_)throw Error("WRONG_THREAD", "Resource tickets require the main thread");
  if(!transfer || transfer->closed() || window<=0 || origin.empty())throw Error("FX_UNBOUND", "Resource transfer requires a bound JSFX document");
  if(!transport_)transport_=std::make_unique<StreamTransport>(*this);
  std::lock_guard<std::mutex> lock(mutex_);
  for(auto it=tickets_.begin();it!=tickets_.end();)
    if(it->second.reliable==transfer)it=tickets_.erase(it);else ++it;
  if(tickets_.size()>=256)throw Error("QUEUE_LIMIT", "Too many native transport tickets");
  const auto token=external_token();Ticket ticket{window,generation,origin,nullptr};ticket.reliable=transfer;
  tickets_.emplace(token,std::move(ticket));
  return {{"token",token},{"url",transport_->url()+"/"+token},{"mode","jsfxResourceV1"}};
}
void StreamHub::detach_external(uint64_t session, const std::string& token) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = tickets_.begin(); it != tickets_.end();)
    if (it->second.external == session && session && (token.empty() || it->first == token)) it = tickets_.erase(it);
    else ++it;
}
Json StreamHub::attach_consumer(const std::string& name, int window, uint64_t generation, const std::string& origin, uint64_t external,
                               const VideoMemoryFactory& video_memory) {
  if (!transport_) transport_ = std::make_unique<StreamTransport>(*this);
  std::lock_guard<std::mutex> lock(mutex_);
  if (tickets_.size() >= 64) throw Error("QUEUE_LIMIT", "At most 64 stream consumers may be attached");
  for (const auto& slot : slots_) if (slot.owned && slot.name == name) {
    if (slot.protected_window && (external || window != slot.protected_window || generation != slot.protected_generation))
      throw Error("FX_UNBOUND", "This stream belongs to another JSFX document");
    std::random_device random; std::string token;
    for (int i = 0; i < 8; ++i) { char text[9]; std::snprintf(text, sizeof(text), "%08x", random()); token += text; }
    auto inserted = tickets_.emplace(token, Ticket{window, generation, origin, slot.owned});
    inserted.first->second.external = external;
    const auto& d = slot.owned->descriptor;
    static const char* kinds[] = {"", "frame", "audio", "spectrum", "meter", "waveform", "binary", "midi"};
    static const char* formats[] = {"", "rgba8", "bgra8", "float32", "bytes", "i420"};
    Json result{{"token", token}, {"url", transport_->url() + "/" + token}, {"name", name}, {"kind", kinds[slot.owned->kind]},
      {"format", formats[d.format]}, {"capacity", d.capacity}, {"maxBytes", d.max_bytes}, {"width", d.width}, {"height", d.height},
      {"stride", d.stride}, {"channels", d.channels}, {"sampleRate", d.sample_rate}, {"blockFrames", d.block_frames},
      {"fftSize", d.fft_size}, {"binHz", d.fft_size ? double(d.sample_rate) / d.fft_size : 0}, {"updateRate", d.update_rate}, {"source", slot.source},
      {"policy", slot.owned->kind == REAWEB_AUDIO || slot.owned->kind == REAWEB_MIDI ? "drop-new" : "latest"}};
    if (slot.protected_window) { result["layoutVersion"] = 2; result["jsfx"] = slot.metadata; }
    if (slot.owned->project_video && d.format == REAWEB_PIXEL_I420) {
      result["colorMatrix"] = slot.metadata["colorMatrix"];
      result["colorRange"] = slot.metadata["colorRange"];
      result["colorConversion"] = slot.metadata["colorConversion"];
    }
    if (slot.owned->project_video && !external && video_memory) {
      size_t allocated = 0;
      for (auto it = video_memory_.begin(); it != video_memory_.end();) {
        if (auto memory = it->lock()) { allocated += size_t(memory->frame_bytes) * 2; ++it; }
        else it = video_memory_.erase(it);
      }
      if (allocated + size_t(d.max_bytes) * 2 <= 256u * 1024 * 1024) {
        try {
          auto memory = video_memory(token, d.max_bytes);
          if (memory && memory->data && memory->frame_bytes == d.max_bytes) {
            if (!memory->resource_url.empty()) result["videoResource"] = memory->resource_url;
            if (memory->script_copy) result["videoNativeCopy"] = true;
            video_memory_.push_back(memory);
            inserted.first->second.video_memory = std::move(memory);
            result["sharedVideo"] = true;
          }
        } catch (...) {} // Keep the binary socket path if native sharing is unavailable.
      }
    }
    return result;
  }
  throw Error("STREAM_NOT_FOUND", "Native stream not registered: " + name);
}
void StreamHub::detach(const std::string& token, int window) {
  std::lock_guard<std::mutex> lock(mutex_); auto it = tickets_.find(token);
  if (it != tickets_.end() && !it->second.external && it->second.window == window) {
    if (it->second.presenter) it->second.presenter->invalidate();
    tickets_.erase(it);
  }
}
void StreamHub::detach_window(int window) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = tickets_.begin(); it != tickets_.end();) {
    if (!it->second.external && it->second.window == window) {
      if (it->second.presenter) it->second.presenter->invalidate();
      it = tickets_.erase(it);
    } else ++it;
  }
}
void StreamHub::detach_stream_window(uint64_t handle, int window) {
  const unsigned index = handle & 255;
  if (!handle || index >= slots_.size()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto& slot = slots_[index];
  if (slot.handle.load() != handle || !slot.owned) return;
  for (auto it = tickets_.begin(); it != tickets_.end();)
    if (!it->second.external && it->second.window == window && it->second.buffer == slot.owned) {
      if (it->second.presenter) it->second.presenter->invalidate();
      it = tickets_.erase(it);
    }
    else ++it;
}
Json StreamHub::info() const {
  std::lock_guard<std::mutex> lock(mutex_); Json streams = Json::array();
  reap();
  for (const auto& slot : slots_) if (slot.owned) streams.push_back({{"name", slot.name}, {"published", slot.owned->published.load()}, {"dropped", slot.owned->dropped.load()}});
  Json result{{"allocatedBytes", allocated_}, {"consumers", tickets_.size()}, {"streams", streams}};
  if (transport_) result["transport"] = transport_->diagnostics();
  return result;
}
size_t StreamHub::consumers(uint64_t handle) const {
  std::lock_guard<std::mutex> lock(mutex_); const auto index = handle & 255;
  if (index >= slots_.size() || slots_[index].handle.load() != handle || !slots_[index].owned) return 0;
  size_t count = 0; for (const auto& ticket : tickets_) if (ticket.second.buffer == slots_[index].owned) ++count;
  return count;
}
double StreamHub::frame_ack_ms(uint64_t handle) const {
  std::lock_guard<std::mutex> lock(mutex_); const auto index = handle & 255;
  if (!handle || index >= slots_.size() || slots_[index].handle.load() != handle || !slots_[index].owned) return -1;
  return slots_[index].owned->frame_ack_ms.load();
}
}
