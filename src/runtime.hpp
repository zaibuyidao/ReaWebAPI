#pragma once
#include "platform.hpp"
#include "worker.hpp"
#include <deque>
#include <set>
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
  bool is_ready(int id) const;
  bool focus(int id);
  Json diagnostics(int id) const;
  bool captures_keyboard(void* handle, const std::function<bool(void*, void*)>& is_child) const;
  void tick();
private:
  struct Session {
    int id = 0, slot = 0;
    fs::path entry;
    fs::path state_path;
    std::string ident, document, title, last_error;
    std::shared_ptr<Window> window;
    std::unique_ptr<Bridge> bridge;
    std::deque<Work> queue;
    std::set<std::string> subscriptions;
    std::map<std::string, Json> events;
    Json last_state, saved_state, pending_state;
    Clock::time_point started = Clock::now(), state_changed = Clock::now();
    Clock::time_point closing_since = Clock::now();
    uint64_t generation = 0, event_sequence = 0, processed = 0;
    size_t outstanding = 0, output_pending = 0;
    bool ready = false, capture_keyboard = true;
    bool closing = false;
    bool failed = false;
  };
  Host host_;
  DockApi dock_;
  fs::path resource_;
  std::function<void(const std::string&)> log_;
  std::unique_ptr<Platform> platform_;
  std::map<int, std::shared_ptr<Session>> sessions_;
  std::map<std::string, Json> state_cache_;
  std::map<int, Json> closed_diagnostics_;
  int next_id_ = 0;
  int cursor_ = 0;
  Worker worker_;
  void* project_ = nullptr;
  uint64_t project_epoch_ = 0, selection_revision_ = 0;
  uint64_t host_generation_ = 0;
  int project_changes_ = -1;
  int selection_index_ = 0, selection_count_ = -1;
  uint64_t selection_hash_ = 0, last_selection_hash_ = 0;
  Clock::time_point next_observation_ = Clock::now();
  Json project_event_, selection_event_;
  bool ticking_ = false;
  std::thread::id main_thread_;
  void check_thread() const;
  void detach(const Session& session);
  Json window_state(const Session& session) const;
  Json host_call(int id, const std::string& method, const Json& args);
  void navigate(Session& session);
  void reply(Session& session, Work work, Json response);
  void emit(Session& session, const std::string& name, Json data);
  void observe(Clock::time_point deadline);
  void persist(Session& session, bool force = false);
  void fail(Session& session, const std::string& message);
};
}
