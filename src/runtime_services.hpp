#pragma once
#include "core.hpp"
#include <memory>
#include <optional>

namespace reaweb {
const std::vector<std::string>& runtime_events();
Json theme_colors(const Host& host);
Json app_info(const fs::path& root, const fs::path& data, const std::string& id);
std::string runtime_platform();
std::string runtime_architecture();
fs::path existing_local_path(const fs::path& base, const Json& input);
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
