#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif
#include "runtime/native_tasks.hpp"
#include <algorithm>
#include <set>

namespace reaweb {
namespace {
struct FileState {
  std::string identity;
  fs::file_time_type time;
  uintmax_t bytes;
  bool directory;
};
using Snapshot = std::map<std::string, FileState>;
FileState file_state(const fs::directory_entry& entry) {
  FileState state{}; state.directory = entry.is_directory();
  state.time = entry.last_write_time(); state.bytes = state.directory ? 0 : entry.file_size();
#ifdef _WIN32
  HANDLE file = CreateFileW(entry.path().c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(file, &info)) state.identity = std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) + ":" + std::to_string(info.nFileIndexLow);
    CloseHandle(file);
  }
#else
  struct stat info{};
  if (!stat(entry.path().c_str(), &info)) state.identity = std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
#endif
  return state;
}
Snapshot scan(const fs::path& path, bool recursive) {
  Snapshot result;
  const auto add = [&](const fs::directory_entry& entry) {
    if (entry.is_symlink() || (!entry.is_directory() && !entry.is_regular_file())) return;
    if (result.size() >= 10000) throw Error("WATCH_LIMIT", "A watch may contain at most 10000 entries");
    result.emplace(entry.path().u8string(), file_state(entry));
  };
  std::error_code error;
  if (!fs::exists(path, error)) return result;
  add(fs::directory_entry(path));
  if (fs::is_directory(path)) {
    if (recursive) for (const auto& entry : fs::recursive_directory_iterator(path)) add(entry);
    else for (const auto& entry : fs::directory_iterator(path)) add(entry);
  }
  return result;
}
}
struct NativeTasks::Watch { uint64_t id; int window; fs::path path; bool recursive; bool ready = false; Snapshot previous; std::string file; };
NativeTasks::NativeTasks() : thread_([this] { run(); }) {}
NativeTasks::~NativeTasks() {
  { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
  wake_.notify_one(); thread_.join();
}
uint64_t NativeTasks::watch(const fs::path& path, bool recursive, int window) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (watches_.size() >= 8) throw Error("WATCH_LIMIT", "At most eight file watches may be active");
  const auto id = ++sequence_; watches_.emplace(id, std::make_shared<Watch>(Watch{id, window, path, recursive}));
  wake_.notify_one(); return id;
}
void NativeTasks::unwatch(uint64_t id, int window) {
  std::lock_guard<std::mutex> lock(mutex_); auto it = watches_.find(id);
  if (it != watches_.end() && it->second->window == window) {
    watches_.erase(it);
    events_.erase(std::remove_if(events_.begin(), events_.end(), [id](const auto& event) { return event.data.at("id") == id; }), events_.end());
  }
}
int NativeTasks::timer(uint32_t delay, uint32_t interval, uint64_t owner, ReaWeb_TimerCallback callback, void* context, uint64_t* handle, int window) {
  if (std::this_thread::get_id() != main_) return REAWEB_MAIN_THREAD_REQUIRED;
  if (!handle || (!callback && !window) || delay > 86400000 || interval > 86400000 || (interval && interval < 10)) return REAWEB_INVALID_ARGUMENT;
  if (timers_.size() >= 256) return REAWEB_QUEUE_LIMIT;
  *handle = ++sequence_; timers_.emplace(*handle, Timer{Clock::now() + std::chrono::milliseconds(delay), interval, owner, callback, context, window});
  return REAWEB_OK;
}
int NativeTasks::cancel(uint64_t id, int window) {
  if (std::this_thread::get_id() != main_) return REAWEB_MAIN_THREAD_REQUIRED;
  auto it = timers_.find(id); if (it != timers_.end() && it->second.window == window) timers_.erase(it); return REAWEB_OK;
}
void NativeTasks::cancel_window(int window) {
  for (auto it = timers_.begin(); it != timers_.end();) if (it->second.window == window) it = timers_.erase(it); else ++it;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = watches_.begin(); it != watches_.end();) if (it->second->window == window) it = watches_.erase(it); else ++it;
  events_.erase(std::remove_if(events_.begin(), events_.end(), [window](const auto& e) { return e.window == window; }), events_.end());
}
void NativeTasks::cancel_owner(uint64_t owner) {
  for (auto it = timers_.begin(); it != timers_.end();) if (it->second.owner == owner) it = timers_.erase(it); else ++it;
}
std::vector<NativeTasks::Event> NativeTasks::tick(Clock::time_point deadline) {
  std::vector<Event> output;
  std::vector<uint64_t> due; const auto now = Clock::now();
  for (const auto& item : timers_) if (item.second.due <= now) due.push_back(item.first);
  for (auto id : due) {
    if (Clock::now() >= deadline) break;
    auto it = timers_.find(id); if (it == timers_.end()) continue;
    const auto timer = it->second;
    if (!timer.interval) timers_.erase(it); else it->second.due = now + std::chrono::milliseconds(timer.interval);
    if (timer.callback) { try { timer.callback(timer.context, id); } catch (...) {} }
    else output.push_back({timer.window, "native-timer", {{"id", id}}});
  }
  std::lock_guard<std::mutex> lock(mutex_);
  while (!events_.empty() && output.size() < 256) {
    auto event = std::move(events_.front()); events_.pop_front();
    if (watches_.count(event.data.at("id").get<uint64_t>())) output.push_back(std::move(event));
  }
  return output;
}
void NativeTasks::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    std::vector<std::shared_ptr<Watch>> watches;
    for (const auto& item : watches_) watches.push_back(item.second);
    lock.unlock();
    for (const auto& watch : watches) {
      std::vector<Json> changes;
      try {
        if (!watch->ready && !fs::is_directory(watch->path)) watch->file = watch->path.u8string();
        auto current = scan(watch->file.empty() ? watch->path : fs::u8path(watch->file).parent_path(), watch->file.empty() && watch->recursive);
        if (!watch->ready) { watch->ready = true; changes.push_back({{"type", "ready"}, {"path", watch->path.u8string()}}); }
        else {
          std::map<std::string, std::string> removed;
          std::set<std::string> renamed;
          for (const auto& old : watch->previous) if (!current.count(old.first) && !old.second.identity.empty()) removed[old.second.identity] = old.first;
          for (const auto& item : current) {
            const auto old = watch->previous.find(item.first);
            if (old == watch->previous.end()) {
              const auto match = removed.find(item.second.identity);
              if (match != removed.end()) { renamed.insert(match->second); changes.push_back({{"type", "renamed"}, {"oldPath", match->second}, {"path", item.first}, {"directory", item.second.directory}}); }
              else changes.push_back({{"type", "created"}, {"path", item.first}, {"directory", item.second.directory}});
            } else if (old->second.time != item.second.time || old->second.bytes != item.second.bytes || old->second.identity != item.second.identity)
              changes.push_back({{"type", "changed"}, {"path", item.first}, {"directory", item.second.directory}});
          }
          for (const auto& item : watch->previous) if (!current.count(item.first) && !renamed.count(item.first))
            changes.push_back({{"type", "deleted"}, {"path", item.first}, {"directory", item.second.directory}});
        }
        watch->previous = std::move(current);
        if (!watch->file.empty()) {
          changes.erase(std::remove_if(changes.begin(), changes.end(), [&](const Json& change) {
            if (change.at("type") == "ready") return false;
            if (change.value("oldPath", std::string()) == watch->file) { watch->file = change.at("path"); return false; }
            return change.value("path", std::string()) != watch->file;
          }), changes.end());
        }
      } catch (const std::exception& error) { changes.push_back({{"type", "error"}, {"code", "WATCH_ERROR"}, {"message", error.what()}}); }
      lock.lock();
      if (watches_.count(watch->id)) for (auto& change : changes) {
        if (events_.size() >= 248) {
          const bool reported = std::any_of(events_.begin(), events_.end(), [&](const auto& event) {
            return event.data.at("id") == watch->id && event.data.value("type", "") == "overflow";
          });
          if (!reported && events_.size() < 256) events_.push_back({watch->window, "file-change", {{"id", watch->id}, {"type", "overflow"}, {"path", watch->path.u8string()}}});
          break;
        }
        change["id"] = watch->id; events_.push_back({watch->window, "file-change", std::move(change)});
      }
      lock.unlock();
    }
    lock.lock(); wake_.wait_for(lock, std::chrono::milliseconds(250));
  }
}
}
