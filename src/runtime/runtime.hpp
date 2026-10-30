#pragma once
#include "platform/platform.hpp"
#include "core/worker.hpp"
#include "web/web_resources.hpp"
#include "runtime/services.hpp"
#include "runtime/host_service.hpp"
#include "runtime/native_monitor.hpp"
#include "runtime/native_stream.hpp"
#include "runtime/native_tasks.hpp"
#include "runtime/native_producers.hpp"
#include "runtime/external_transport.hpp"
#include "runtime/external_settings.hpp"
#include "runtime/diagnostics.hpp"
#include "runtime/jsfx_webui.hpp"
#include "runtime/jsfx_control.hpp"
#include <deque>
#include <optional>
#include <set>
#include <thread>

namespace reaweb {
// A frame is pushed on this cadence, independent of how often the timer runs and
// of how often Lua updates its content. Lua owns what a frame contains; the
// extension owns when the page sees it. Diagnostics report this same value.
//
// Why 8: the timer already paces this channel, so the value is only a floor
// against a burst, and a floor above the timer's own period deletes real frames.
// A timer probe over 25 runs measured a median period of 31.7 ms and a minimum
// of 10.16 ms, the minimum being the pass REAPER runs right after a 130 to 141 ms
// stall. 16 ms sat above that minimum and dropped exactly those passes, which are
// the first frames after a stall. 8 ms is below every period in all 25 runs, so
// it drops nothing the timer delivered, and it still bounds a faster burst.
constexpr auto frame_interval = std::chrono::milliseconds(8);
class Runtime {
public:
  Runtime(Host host, fs::path resource, std::function<void(const std::string&)> log, DockApi dock = {});
  ~Runtime();
  void schedule_webview_warmup() { check_thread(); platform_warmup_pending_ = true; }
  void configure_webviews(unsigned idle_count);
  int open(const std::string& path, const fs::path& base = {});
  int open_instance(const std::string& path, const std::string& instance_key, const std::string& name = {}, bool multiple = false, bool hidden = false, bool frameless = false, bool resizable = true, bool transparent = false);
  int open_ex(const std::string& path, const std::string& options);
  int open_dev(const std::string& url, const fs::path& base = {});
  bool is_open(int id) const;
  bool close(int id);
  bool devtools(int id);
  bool set_docked(int id, bool docked);
  bool is_docked(int id) const;
  bool is_ready(int id) const;
  bool focus(int id);
  bool show(int id);
  bool send(int id, const std::string& message);
  std::string receive(int id);
  bool set_frame(int id, const std::string& text);
  Json diagnostics(int id) const;
  bool captures_keyboard(void* handle, const std::function<bool(void*, void*)>& is_child, bool daw_only = false) const;
  void tick();
  ServiceRegistry& services() { return services_; }
  StreamHub& streams() { return streams_; }
  NativeTasks& tasks() { return tasks_; }
  NativeProducers& producers() { return producers_; }
  void configure_diagnostics(const DiagnosticSettings& settings) { check_thread(); diagnostic_log_.configure(settings); }
  void open_diagnostics_folder();
  void configure_external(const ExternalSettings& settings);
  void initialize_jsfx(JsfxDiscovery discovery);
  uint16_t external_port() const;
private:
  struct ExternalSession {
    struct Subscription { std::string event, service; uint64_t handle = 0; };
    uint64_t id = 0, sequence = 0;
    std::string identity;
    std::shared_ptr<ExternalConnection> connection;
    std::unique_ptr<ReaperApiCore> core;
    std::map<std::string, Subscription> subscriptions;
    std::map<std::string, Json> events;
    std::map<std::string, std::string> consumers;
  };
  std::unique_ptr<ExternalTransport> external_transport_;
  ExternalSettings external_settings_;
  std::map<uint64_t, std::shared_ptr<ExternalSession>> external_sessions_;
  uint64_t next_external_ = 0, external_cursor_ = 0;
  bool external_turn_ = false;
  void sync_external();
  void stop_external();
  bool dispatch_external();
  void external_event(const std::string& name, const Json& data);
  void external_service_event(uint64_t handle, const std::string& service, int window, const std::string& name, const Json& data);
  Json external_event_data(ExternalSession& session, const std::string& name, Json data);
  void flush_external_events();
  struct App {
    std::string id, origin, mode, launcher_source;
    Json info;
    std::shared_ptr<WebResources> resources;
    std::shared_ptr<Platform> platform;
  };
  struct JsfxInstance {
    std::unique_ptr<JsfxBinding> binding;
    std::string session_id = external_token();
    uint64_t revision = 0;
    std::unique_ptr<JsfxControl> native;
    std::string native_error;
    Clock::time_point last_used = Clock::now();
  };
  struct Session {
    int id = 0, slot = 0;
    fs::path entry;
    fs::path state_path;
    std::string ident, document, title, default_title, last_error;
    bool title_explicit = false;
    std::optional<std::pair<std::string, std::string>> instance;
    std::shared_ptr<App> app;
    std::shared_ptr<Window> window;
    std::shared_ptr<StartupTiming> startup_timing = std::make_shared<StartupTiming>();
    std::unique_ptr<Bridge> bridge;
    std::deque<Work> queue;
    std::deque<std::string> to_web, to_host;
    size_t message_bytes = 0, web_messages = 0;
    std::set<std::string> subscriptions;
    std::map<uint64_t, std::set<std::string>> service_subscriptions;
    std::map<std::string, Json> events;
    Json last_state, saved_state, pending_state;
    // Frame channel. The extension pushes on its own cadence; Lua only supplies
    // the content it owns. Keeping these separate is what stops a slow defer
    // from stalling the page.
    std::string lua_frame;
    Clock::time_point lua_frame_at = Clock::now(), next_frame = Clock::now();
    uint64_t frame_sequence = 0, frames_pushed = 0, lua_frame_writes = 0;
    Clock::time_point started = Clock::now(), state_changed = Clock::now();
    Clock::time_point closing_since = Clock::now();
    uint64_t generation = 0, event_sequence = 0, processed = 0;
    size_t outstanding = 0, output_pending = 0;
    bool diagnostic_host_connected = false, diagnostic_jsfx_released = false;
    bool ready = false, capture_keyboard = true;
    bool daw_keyboard = false, space_down = false;
    std::deque<char> space_events;
    bool closing = false;
    bool background_start = false, pending_dock = false;
    bool frameless = false, resizable = true;
    double corner_radius = 0;
    bool transparent = false;
    std::optional<bool> native_dock_request;
    std::shared_ptr<const IconSource> icon_source;
    std::vector<IconBitmap> icon_bitmaps;
    std::vector<int> icon_sizes;
    void* icon_target = nullptr;
    uint64_t icon_sequence = 0;
    bool icon_pending = false;
    bool icon_explicit = false;
    bool icon_explicit_source = false;
    bool icon_visible = true, icon_dirty = false, icon_initialized = false;
    uint64_t favicon_revision = 0;
    bool failed = false;
    struct Jsfx {
      JsfxEditor editor;
      std::shared_ptr<JsfxInstance> instance;
      Json values = Json::array(), changes = Json::array();
      int scan = 0, count = 0;
      std::vector<int> editing;
      std::string gesture_id;
      bool group_edit = false;
      bool daw_keyboard = true;
      uint64_t host_revision = 0, activation = 1;
      bool initialized = false, presented = false;
      double presentation_ms = 0;
      std::string undo;
      Clock::time_point next_poll{}, last_edit{}, last_used = Clock::now();
      Clock::time_point discovered = Clock::now();
    };
    std::shared_ptr<Jsfx> jsfx;
    bool daw_keyboard_enabled() const { return daw_keyboard && (!jsfx || bool(jsfx->editor.host)); }
    bool lifecycle_enabled = false, allow_reload = false;
    std::string lifecycle_action, lifecycle_token;
    Clock::time_point lifecycle_deadline;
    Json logs = Json::array();
    struct Audio { Work request; std::unique_ptr<AudioJob> job; };
    std::deque<Audio> audio;
  };
  Host host_;
  ServiceRegistry services_;
  StreamHub streams_;
  std::unique_ptr<JsfxNativeHub> jsfx_native_;
  NativeTasks tasks_;
  NativeProducers producers_;
  NativeMonitorManager monitors_;
  DockApi dock_;
  fs::path resource_;
  std::function<void(const std::string&)> log_;
  Diagnostics diagnostic_log_;
  std::weak_ptr<Platform> platform_;
  std::shared_ptr<Platform> warmed_platform_;
  bool platform_warmup_pending_ = false;
  unsigned idle_webviews_ = 0;
  std::shared_ptr<Platform> prepare_platform(const std::string& app = {});
  std::map<std::string, std::weak_ptr<App>> apps_;
  std::map<int, std::shared_ptr<Session>> sessions_;
  std::map<std::string, Json> state_cache_;
  std::map<int, Json> closed_diagnostics_;
  int next_id_ = 0;
  int cursor_ = 0;
  int message_cursor_ = 0;
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
  uint64_t host_selection_revision_ = 0;
  int project_changes_ = -1;
  int selection_index_ = 0, selection_count_ = -1;
  bool selection_pending_ = true;
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
  Clock::time_point next_devices_ = Clock::now();
  uint64_t lifecycle_sequence_ = 0;
  bool ticking_ = false;
  // Set by receive() when it pumped the WebView IPC and dispatched requests,
  // cleared when a new timer pass starts. A Lua pass polls up to 32 times, and
  // only the first of those may pay for a pump and a dispatch.
  bool receive_dispatched_ = false;
  std::thread::id main_thread_;
  JsfxDiscovery jsfx_discovery_;
  std::shared_ptr<Platform> jsfx_platform_;
  uint64_t jsfx_service_ = 0;
  int jsfx_poll_cursor_ = 0;
  Clock::time_point next_jsfx_discovery_{};
  std::map<void*, std::shared_ptr<JsfxEditorHost>> jsfx_blocked_;
  std::vector<std::shared_ptr<JsfxInstance>> jsfx_instances_;
  struct JsfxSize {
    JsfxEditorDiscovery::Size size;
    uint64_t revision = 1;
  };
  std::map<std::string, JsfxSize> jsfx_sizes_;
  JsfxSize& jsfx_size(const JsfxAssociation&);
  Json jsfx_size_state(Session&);
  void tick_jsfx();
  void release_jsfx(Session& session);
  void park_jsfx(Session& session);
  void attach_jsfx(Session& session, const JsfxEditor& editor);
  Json jsfx_state(Session& session);
  Json jsfx_call(int window, const std::string& method, const Json& payload);
  Json jsfx_result(int window, const std::string& method, const Json& payload);
  void end_jsfx_edit(Session& session);
  void release_daw_space(Session& session);
  bool daw_keyboard_default(Session& session);
  void tick_daw_keyboard();
  void check_thread() const;
  void enqueue_message(Session& session, const std::string& message, bool to_web);
  void clear_messages(Session& session);
  void flush_messages(Session& session, Clock::time_point deadline);
  int open_impl(const std::string& path, const fs::path& base, const std::string& dev_url, const std::string& launcher_source = {}, bool hidden = false, bool frameless = false, bool resizable = true, bool transparent = false, std::shared_ptr<Session::Jsfx> jsfx = {});
  void attach_pending_dock(Session& session);
  bool start_async(Session& session, const Work& request);
  void refresh_icon(Session& session);
  void set_title(Session& session, const std::string& title);
  void finish_undo();
  Json transaction_call(int id, const std::string& method, const Json& args);
  void detach(const Session& session);
  Json window_state(const Session& session) const;
  Json web_runtime(const Session& session) const;
  Json host_call(int id, const std::string& method, const Json& args);
  void navigate(Session& session);
  void reply(Session& session, Work work, Json response);
  bool deliver_inline(Session& session, const Work& work, const Json& response);
  void emit(Session& session, const std::string& name, Json data);
  void observe(Clock::time_point deadline);
  void observe_extra(Clock::time_point deadline, int changes);
  void observe_native(Clock::time_point deadline);
  bool service_call(Session& session, const Work& request);
  void service_event(uint64_t handle, const std::string& service, int window, const std::string& name, Json data);
  bool lifecycle(Session& session, const std::string& action);
  bool defer_reload(int id);
  void complete_lifecycle(Session& session);
  void trace(const Session& session, const char* event, Diagnostics::Level level = Diagnostics::Level::Debug,
    const char* result = "ok", Diagnostics::Scope scope = Diagnostics::Scope::Runtime) noexcept;
  void append_log(Session& session, Json entry);
  void persist(Session& session, bool force = false);
  void fail(Session& session, const std::string& message);
  void pump_frames();
  void dispatch_requests(Clock::time_point deadline);
};
}
