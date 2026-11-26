#include "runtime/services.hpp"
#include <map>

namespace reaweb {
namespace {
const std::map<std::string, std::string> aliases = {
  {"projectchange", "project-changed"},
  {"selectionchange", "track-selection-changed"},
  {"itemselectionchange", "item-selection-changed"},
  {"takeselectionchange", "take-selection-changed"},
  {"transportchange", "transport-changed"},
  {"fxchange", "fx-focus-touch-state-changed"},
  {"windowstatechange", "window-state-changed"},
  {"trackStateChanged", "track-state-changed"},
  {"transportChanged", "transport-state-changed"},
  {"projectChanged", "open-projects-state-changed"},
  {"markersChanged", "marker-list-changed"},
  {"regionsChanged", "region-list-changed"},
  {"currentRegionChanged", "current-region-changed"},
  {"loopPointsChanged", "loop-points-changed"},
  {"timeSelectionChanged", "time-selection-changed"},
  {"devicesChanged", "devices-changed"}
};
}

const std::string* runtime_event_alias(const std::string& source) {
  const auto found = aliases.find(source);
  return found == aliases.end() ? nullptr : &found->second;
}

const std::string& runtime_event_source(const std::string& name) {
  static const auto sources = [] {
    std::map<std::string, std::string> result;
    for (const auto& alias : aliases) result.emplace(alias.second, alias.first);
    return result;
  }();
  const auto found = sources.find(name);
  return found == sources.end() ? name : found->second;
}

std::vector<std::string> with_runtime_event_aliases(std::vector<std::string> names) {
  const auto count = names.size();
  for (size_t i = 0; i < count; ++i)
    if (const auto* alias = runtime_event_alias(names[i])) names.push_back(*alias);
  return names;
}

const std::vector<std::string>& runtime_events() {
  static const auto names = with_runtime_event_aliases({"projectchange", "selectionchange", "itemselectionchange",
    "takeselectionchange", "transportchange", "fxchange", "windowstatechange", "track-added", "track-deleted",
    "track-selected", "item-changed", "take-changed", "playback-state-changed", "tempo-changed",
    "marker-changed", "fx-changed", "project-loaded", "project-saved", "theme-changed", "native-drop", "message",
    "trackSelectionChanged", "trackStateChanged", "transportChanged", "projectChanged", "markersChanged",
    "regionsChanged", "currentRegionChanged", "loopPointsChanged", "timeSelectionChanged", "devicesChanged", "file-change", "native-timer",
    "frame"});
  return names;
}
}
