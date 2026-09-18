#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <functional>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace reaweb {
using Json = nlohmann::json;
using Guid = std::array<unsigned char, 16>;
namespace fs = std::filesystem;

struct Error : std::runtime_error {
  std::string code;
  Json details = nullptr;
  Error(std::string code, const std::string& message, Json details = nullptr)
    : std::runtime_error(message), code(std::move(code)), details(std::move(details)) {}
};

struct Host {
  std::function<void*()> current_project;
  std::function<int(void*)> count_tracks;
  std::function<int(void*)> count_selected_tracks;
  std::function<void*(void*, int)> get_track;
  std::function<void*(void*, int)> get_selected_track;
  std::function<bool(void*, void*)> valid_track;
  std::function<Guid(void*)> track_guid;
  std::function<std::string(void*)> track_name;
  std::function<double(void*, const std::string&)> get_track_value;
  std::function<bool(void*, const std::string&, double)> set_track_value;
  std::function<std::string()> version;
  std::function<int(void*)> change_count;
  std::function<uint64_t()> project_generation;
  std::function<void(void*)> begin_undo;
  std::function<void(void*, const std::string&)> end_undo;
  std::function<void(int)> prevent_refresh;
  std::function<void()> update_arrange;
};

fs::path resolve_html(const fs::path& base, const std::string& input);
std::string file_uri(const fs::path& path);
bool same_document(const std::string& uri, const std::string& entry);
Json parse_request(const std::string& message);
Json error_response(const Json& request, const std::string& code, const std::string& message, Json details = nullptr);

class Bridge {
public:
  struct Controls {
    std::function<int(const std::string&)> open;
    std::function<void()> close;
    std::function<void()> devtools;
    std::function<bool(bool)> set_docked;
    std::function<bool()> is_docked;
    std::function<Json(const std::string&, const Json&)> host_call;
  };
  Bridge(Host& host, Controls controls, std::string session);
  Json dispatch(const std::string& message);
  Json dispatch_request(const Json& request);
  void observe_project();
  void reset_handles();
private:
  struct Handle { void* pointer; void* project; Guid guid; };
  struct Method { size_t min_args, max_args; std::function<Json(const Json&)> invoke; };
  Host& host_;
  Controls controls_;
  std::string session_;
  void* project_ = nullptr;
  uint64_t next_handle_ = 0;
  std::unordered_map<std::string, Handle> handles_;
  std::unordered_map<void*, std::string> track_tokens_;
  std::deque<std::string> handle_scan_;
  std::map<std::string, Method> methods_;
  bool batching_ = false;
  void add(const std::string& name, size_t min, size_t max, std::function<Json(const Json&)> fn);
  void* project(const Json& args);
  Json track_handle(void* pointer);
  void* track(const Json& value);
  Json batch(const Json& args);
};
}
