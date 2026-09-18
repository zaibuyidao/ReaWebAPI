#pragma once
#include "core.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace reaweb {
using Clock = std::chrono::steady_clock;
// Only value objects cross this boundary. REAPER and WebView objects stay on the main thread.
struct Work {
  enum Kind { Parse, Encode, Save, Request, Script, Fault } kind = Parse;
  int session = 0;
  uint64_t generation = 0, project = 0;
  Clock::time_point received = Clock::now();
  std::string text;
  Json data;
  fs::path path;
  bool reply = false;
  bool counted_output = false;
};
class Worker {
public:
  Worker();
  ~Worker();
  bool submit(Work work);
  bool take(Work& work);
private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Work> input_, output_;
  size_t input_bytes_ = 0, output_bytes_ = 0;
  bool stopping_ = false;
  std::thread thread_;
  void run();
};
std::string state_key(const std::string& entry, int slot);
Json read_state(const fs::path& path, const std::string& entry);
}
