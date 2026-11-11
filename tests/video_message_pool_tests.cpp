#include "runtime/video_message_pool.hpp"
#include <iostream>
#include <stdexcept>

using namespace reaweb;
struct Message { std::vector<unsigned char> video; };
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

int main() {
  try {
    // Small frames may use more snapshots, but a publication reserve remains.
    VideoMessagePool<Message> pool(384);
    std::vector<std::shared_ptr<Message>> held;
    for (int i = 0; i < 24; ++i) {
      std::vector<unsigned char> packet(16, static_cast<unsigned char>(i));
      auto message = pool.acquire(packet.capacity()); CHECK(message);
      message->video.swap(packet); held.push_back(message);
      CHECK(pool.capacity() <= 384);
    }
    CHECK(pool.capacity() == 384);
    CHECK(!pool.acquire(16));
    CHECK(pool.can_pin(22 * 16, 16));
    CHECK(!pool.can_pin(23 * 16, 16));
    CHECK(!pool.can_pin(385, 0));
    CHECK(held.front()->video.front() == 0 && held.back()->video.front() == 23);
    held.clear(); pool.release_unused(); CHECK(pool.capacity() == 0);

    // Charge reserved capacity even when the current payload is smaller.
    std::vector<unsigned char> packet; packet.reserve(64); packet.resize(16, 91);
    VideoMessagePool<Message> capacity_pool(96);
    auto large = capacity_pool.acquire(packet.capacity()); CHECK(large);
    large->video.swap(packet); CHECK(capacity_pool.capacity() == 64);
    CHECK(!capacity_pool.acquire(48));
    CHECK(large->video.size() == 16 && large->video.front() == 91);
    large.reset();
    auto resized = capacity_pool.acquire(48); CHECK(resized);
    CHECK(resized->video.capacity() == 0);
    std::vector<unsigned char> replacement(48, 72);
    resized->video.swap(replacement);
    CHECK(replacement.capacity() == 0 && capacity_pool.capacity() == 48);

    // A profile change frees unused allocations while preserving partial old frames.
    VideoMessagePool<Message> profiles(384);
    std::vector<std::shared_ptr<Message>> previous;
    for (int i = 0; i < 3; ++i) {
      std::vector<unsigned char> old(128, 81);
      auto message = profiles.acquire(old.capacity()); CHECK(message);
      message->video.swap(old); previous.push_back(message);
    }
    profiles.release_unused(); CHECK(profiles.capacity() == 384);
    CHECK(!profiles.acquire(16));
    previous.pop_back(); profiles.release_unused(); CHECK(profiles.capacity() == 256);
    std::vector<unsigned char> next(16, 42);
    auto current = profiles.acquire(next.capacity()); CHECK(current);
    current->video.swap(next);
    CHECK(next.capacity() == 0 && profiles.capacity() == 272);
    CHECK(previous.front()->video.size() == 128 && previous.front()->video.front() == 81);
    previous.clear(); profiles.release_unused(); CHECK(profiles.capacity() == 16);
    current.reset();
    std::vector<unsigned char> same_size(16, 43);
    auto reused = profiles.acquire(same_size.capacity()); CHECK(reused);
    reused->video.swap(same_size);
    CHECK(same_size.capacity() == 16 && same_size.front() == 42);
    CHECK(profiles.capacity() == 16);
    std::cout << "Video payload capacity, publication reserve and profile changes passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
