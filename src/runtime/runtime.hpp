#pragma once
#include "platform/platform.hpp"
#include "core/worker.hpp"
#include "web/web_resources.hpp"
#include "runtime/services.hpp"
#include <deque>
#include <set>
#include <thread>

namespace reaweb {
class Runtime {
public:
  Runtime(Host host, fs::path resource, std::function<void(const std::string&)> log, DockApi dock = {});
  ~Runtime();
  int open(const std::string& path, const fs::path& base = {});
  int open_dev(const std::string& url, const fs::path& base = {});
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
  struct App {
    std::string id, origin, mode;
    Json info;
    std::unique_ptr<WebResources> resources;
    std::unique_ptr<Platform> platform;
  };
  struct Session {
    int id = 0, slot = 0;
    fs::path entry;
    fs::path state_path;
    std::string ident, document, title, last_error;
    std::shared_ptr<App> app;
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
    bool lifecycle_enabled = false, allow_reload = false;
    std::string lifecycle_action, lifecycle_token;
    Clock::time_point lifecycle_deadline;
    Json logs = Json::array();
    struct Audio { Work request; std::unique_ptr<AudioJob> job; };
    std::deque<Audio> audio;
  };
  Host host_;
  DockApi dock_;
  fs::path resource_;
  std::function<void(const std::string&)> log_;
  std::map<std::string, std::weak_ptr<App>> apps_;
  std::map<int, std::shared_ptr<Session>> sessions_;
  std::map<std::string, Json> state_cache_;
  std::map<int, Json> closed_diagnostics_;
  int next_id_ = 0;
  int cursor_ = 0;
  Worker worker_;
  int undo_owner_ = 0;
  int drag_owner_ = 0;
  uint64_t undo_sequence_ = 0;
  std::string undo_token_, undo_label_;
  void* undo_project_ = nullptr;
  Clock::time_point undo_deadline_;
  void* project_ = nullptr;
  uint64_t project_epoch_ = 0, selection_revision_ = 0;
  uint64_t host_generation_ = 0;
  int project_changes_ = -1;
  int selection_index_ = 0, selection_count_ = -1;
  uint64_t selection_hash_ = 0, last_selection_hash_ = 0;
  Clock::time_point next_observation_ = Clock::now();
  Json project_event_, selection_event_;
  Json item_event_, take_event_, transport_event_, fx_event_;
  int item_index_ = 0, item_count_ = -1, take_count_ = 0;
  uint64_t item_hash_ = 0, take_hash_ = 0, last_item_hash_ = 0, last_take_hash_ = 0;
  uint64_t item_revision_ = 0, take_revision_ = 0;
  std::map<std::string, Json> extra_events_;
  std::map<std::string, uint64_t> extra_revisions_;
  std::vector<std::string> track_scan_, last_tracks_;
  int track_scan_count_ = -1, extra_change_ = -1;
  uint64_t extra_epoch_ = 0, track_revision_ = 0;
  bool tracks_initialized_ = false;
  Json saved_project_state_;
  Clock::time_point next_theme_ = Clock::now();
  uint64_t lifecycle_sequence_ = 0;
  bool ticking_ = false;
  std::thread::id main_thread_;
  void check_thread() const;
  int open_impl(const std::string& path, const fs::path& base, const std::string& dev_url);
  bool start_async(Session& session, const Work& request);
  void finish_undo();
  Json transaction_call(int id, const std::string& method, const Json& args);
  void detach(const Session& session);
  Json window_state(const Session& session) const;
  Json web_runtime(const Session& session) const;
  Json host_call(int id, const std::string& method, const Json& args);
  void navigate(Session& session);
  void reply(Session& session, Work work, Json response);
  void emit(Session& session, const std::string& name, Json data);
  void observe(Clock::time_point deadline);
  void observe_extra(Clock::time_point deadline, int changes);
  bool lifecycle(Session& session, const std::string& action);
  bool defer_reload(int id);
  void complete_lifecycle(Session& session);
  void append_log(Session& session, Json entry);
  void persist(Session& session, bool force = false);
  void fail(Session& session, const std::string& message);
};
}
