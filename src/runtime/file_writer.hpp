#pragma once
#include "core/worker.hpp"
#include <atomic>

namespace reaweb {
class FileWriters {
public:
  static constexpr size_t chunk_limit = 1024 * 1024;
  static constexpr uint64_t file_limit = 64ull * 1024 * 1024 * 1024;
  static bool handles(const std::string& method);
  FileWriters();
  ~FileWriters();
  void submit(Work request);
  bool take(Work& response);
  void cancel_window(int window);
private:
  struct Owner { std::atomic<bool> cancelled{false}; bool busy = false; };
  struct Job { Work work; std::shared_ptr<Owner> owner; bool data; };
  struct Entry;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::map<std::pair<int, uint64_t>, std::shared_ptr<Owner>> owners_;
  std::deque<Job> input_;
  std::deque<Work> output_;
  std::map<std::string, std::unique_ptr<Entry>> entries_;
  size_t writes_ = 0;
  bool stopping_ = false;
  uint64_t sequence_ = 0;
  std::thread thread_;
  Json execute(Job& job);
  void reap();
  void run();
};
}
