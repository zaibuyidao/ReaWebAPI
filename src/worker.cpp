#include "worker.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace reaweb {
namespace {
constexpr size_t byte_limit = 16 * 1024 * 1024;
size_t json_cost(const Json& value) {
  size_t size = 64;
  if (value.is_string()) return size + value.get_ref<const std::string&>().size() * 6;
  if (value.is_structured()) for (const auto& item : value.items())
    size += (value.is_object() ? item.key().size() * 6 : 0) + json_cost(item.value());
  return size;
}
size_t cost(const Work& work) { return work.text.size() + json_cost(work.data) + 256; }
void save(const fs::path& path, const Json& data) {
  fs::create_directories(path.parent_path());
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream << data.dump(2) << '\n';
    stream.flush();
    if (!stream) throw std::runtime_error("Cannot write window state: " + path.u8string());
  }
#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Cannot replace window state: " + path.u8string());
#else
  fs::rename(temporary, path);
#endif
}
}
Worker::Worker() : thread_([this] { run(); }) {}
Worker::~Worker() {
  { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
  changed_.notify_all();
  thread_.join();
}
bool Worker::submit(Work work) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || input_.size() >= 1024 || input_bytes_ + cost(work) > byte_limit) return false;
  input_bytes_ += cost(work);
  input_.push_back(std::move(work));
  changed_.notify_one();
  return true;
}
bool Worker::take(Work& work) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (output_.empty()) return false;
  work = std::move(output_.front());
  output_bytes_ -= cost(work);
  output_.pop_front();
  changed_.notify_one();
  return true;
}
void Worker::run() {
  for (;;) {
    Work work;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return stopping_ || !input_.empty(); });
      if (input_.empty()) return;
      work = std::move(input_.front());
      input_bytes_ -= cost(work);
      input_.pop_front();
      if (stopping_ && work.kind != Work::Save) continue;
    }
    try {
      if (work.kind == Work::Save) { save(work.path, work.data); continue; }
      if (work.kind == Work::Parse) {
        try { work.data = parse_request(work.text); work.kind = Work::Request; work.text.clear(); }
        catch (const Error& e) { work.data = error_response(Json(), e.code, e.what()); work.kind = Work::Encode; }
        catch (const Json::exception&) { work.data = error_response(Json(), "INVALID_REQUEST", "Invalid JSON request"); work.kind = Work::Encode; }
      }
      if (work.kind == Work::Encode) {
        auto encoded = work.data.dump(-1, ' ', true, Json::error_handler_t::replace);
        if (encoded.size() > 512 * 1024) {
          encoded = error_response(work.data, "RESPONSE_LIMIT", "Response exceeds 512 KiB. Use a smaller batch.").dump();
        }
        work.text = "window.__reawebReceive(" + encoded + ");";
        work.data = nullptr;
        work.kind = Work::Script;
      }
    } catch (const std::exception& e) { work.kind = Work::Fault; work.text = e.what(); work.data = nullptr; }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this, &work] { return stopping_ ||
        (output_.size() < 1024 && output_bytes_ + cost(work) <= byte_limit); });
      if (stopping_) continue;
      output_bytes_ += cost(work);
      output_.push_back(std::move(work));
    }
  }
}
std::string state_key(const std::string& entry, int slot) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char ch : entry) { hash ^= ch; hash *= 1099511628211ull; }
  std::ostringstream stream;
  stream << std::hex << std::setw(16) << std::setfill('0') << hash << '-' << std::dec << slot;
  return stream.str();
}
Json read_state(const fs::path& path, const std::string& entry) {
  std::error_code error;
  if (!fs::exists(path, error)) return Json();
  if (fs::file_size(path, error) > 65536 || error) throw Error("STATE_INVALID", "Window state file is too large");
  std::ifstream stream(path, std::ios::binary);
  auto state = Json::parse(stream, [](int depth, Json::parse_event_t, Json&) {
    if (depth > 8) throw Error("STATE_INVALID", "Window state nesting exceeds limit");
    return true;
  });
  if (!state.is_object() || state.value("schema", 0) != 1 || state.value("entry", "") != entry ||
      !state.contains("placement") || !state["placement"].is_object())
    throw Error("STATE_INVALID", "Invalid window state file");
  const auto& placement = state["placement"];
  for (const auto* name : {"x", "y", "width", "height"}) {
    if (!placement.contains(name) || !placement[name].is_number_integer()) throw Error("STATE_INVALID", "Invalid window bounds");
    const auto number = placement[name].get<double>();
    if (number < -1000000 || number > 1000000) throw Error("STATE_INVALID", "Window bounds exceed limit");
  }
  if (placement["width"] < 100 || placement["width"] > 100000 || placement["height"] < 100 || placement["height"] > 100000 ||
      !placement.value("maximized", Json(false)).is_boolean() || !state.value("docked", Json(false)).is_boolean() ||
      !state.value("dockId", Json(-1)).is_number_integer() || state.value("dockId", -1) < -1 || state.value("dockId", -1) > 15)
    throw Error("STATE_INVALID", "Invalid saved window state");
  return state;
}
}
