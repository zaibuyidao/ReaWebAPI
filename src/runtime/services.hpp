#pragma once
#include "core/core.hpp"
#include "runtime/fs.hpp"
#include <memory>
#include <optional>

namespace reaweb {
const std::vector<std::string>& runtime_events();
const std::string* runtime_event_alias(const std::string& source);
const std::string& runtime_event_source(const std::string& name);
std::vector<std::string> with_runtime_event_aliases(std::vector<std::string> names);
Json theme_colors(const Host& host);
Json app_info(const fs::path& root, const fs::path& data, const std::string& id);
bool app_daw_keyboard(const fs::path& root);
bool app_cross_origin_isolation(const fs::path& root);
bool app_video_high_refresh_rate(const fs::path& root);
std::string runtime_platform();
std::string runtime_architecture();
Json system_diagnostics();
Json drag_payload(const fs::path& base, const std::string& method, const Json& args);

// Owned sources and all REAPER calls stay on the main thread. Peak building is
// advanced one native step per tick, never in a browser or file worker thread.
class AudioJob {
public:
  AudioJob(const Host& host, const fs::path& base, const Json& args, bool waveform);
  ~AudioJob();
  std::optional<Json> step();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
