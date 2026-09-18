#pragma once
#include "platform.hpp"
#include <deque>
#include <thread>

namespace reaweb {
class Runtime {
public:
  Runtime(Host host, fs::path resource, std::function<void(const std::string&)> log, DockApi dock = {});
  ~Runtime();
  int open(const std::string& path, const fs::path& base = {});
  bool is_open(int id) const;
  bool close(int id);
  bool devtools(int id);
  bool set_docked(int id, bool docked);
  bool is_docked(int id) const;
  void tick();
private:
  struct Session {
    fs::path entry;
    std::shared_ptr<Window> window;
    std::unique_ptr<Bridge> bridge;
    std::deque<std::string> queue;
    bool closing = false;
  };
  Host host_;
  DockApi dock_;
  fs::path resource_;
  std::function<void(const std::string&)> log_;
  std::unique_ptr<Platform> platform_;
  std::map<int, std::shared_ptr<Session>> sessions_;
  int next_id_ = 0;
  bool ticking_ = false;
  std::thread::id main_thread_;
  void check_thread() const;
  void detach(const Session& session);
};
}
