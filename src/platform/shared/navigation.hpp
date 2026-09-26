#pragma once
#include "core/core.hpp"

namespace reaweb {
// Native WebViews supply absolute URLs. The App/dev origin is local even when
// its documents are served over HTTP. Keep its path and query changes blocked.
inline bool local_navigation(const std::string& uri, const std::string& entry) {
  if (uri.rfind("file:", 0) == 0) return true;
  const auto scheme = entry.find("://");
  if (scheme == std::string::npos) return false;
  const auto origin = entry.substr(0, entry.find_first_of("/?#", scheme + 3));
  return uri.compare(0, origin.size(), origin) == 0 &&
    (uri.size() == origin.size() || uri[origin.size()] == '/' ||
     uri[origin.size()] == '?' || uri[origin.size()] == '#');
}

// Called only after the native navigation decision has been cancelled.
template<class Open, class Evaluate>
void blocked_navigation(const std::string& uri, const std::string& entry, Open open, Evaluate evaluate) {
  if (same_document(uri, entry)) return;
  std::string warning;
  if (local_navigation(uri, entry)) {
    warning = "ReaWebAPI: Navigation to another local document or query is blocked. Use reaper.window.open(path) to open local HTML.";
  } else {
    try { validate_external_url(uri); open(uri); }
    catch (const std::exception& error) { warning = std::string("ReaWebAPI: ") + error.what(); }
  }
  if (!warning.empty()) evaluate("console.warn(" + Json(warning).dump() + ");");
}
}
