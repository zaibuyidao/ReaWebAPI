#pragma once
#include <algorithm>
#include <memory>
#include <vector>

namespace reaweb {
// Payload capacity follows the buffers exchanged with the producer, not their size.
template<class Message> class VideoMessagePool {
  size_t budget_, reserve_ = 0;
  std::vector<std::shared_ptr<Message>> messages_;
public:
  explicit VideoMessagePool(size_t budget) : budget_(budget) {}
  size_t capacity() const {
    size_t bytes = 0;
    for (const auto& message : messages_) bytes += message->video.capacity();
    return bytes;
  }
  bool can_pin(size_t pinned, size_t additional) const {
    return pinned <= budget_ && additional <= budget_ - pinned && reserve_ <= budget_ - pinned - additional;
  }
  std::shared_ptr<Message> acquire(size_t incoming) {
    reserve_ = incoming;
    if (incoming > budget_) return {};
    for (const auto& message : messages_) if (message.use_count() == 1) {
      // Do not return a previous profile's larger allocation to the producer ring.
      if (message->video.capacity() != incoming) decltype(message->video)().swap(message->video);
      if (incoming <= budget_ - (capacity() - message->video.capacity())) return message;
    }
    if (incoming > budget_ - capacity()) return {};
    auto message = std::make_shared<Message>(); messages_.push_back(message); return message;
  }
  void release_unused() {
    messages_.erase(std::remove_if(messages_.begin(), messages_.end(),
      [](const auto& message) { return message.use_count() == 1; }), messages_.end());
  }
};
}
