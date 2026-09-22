#include "runtime/services.hpp"

namespace reaweb {
const std::vector<std::string>& runtime_events() {
  static const std::vector<std::string> names = {"projectchange", "selectionchange", "itemselectionchange",
    "takeselectionchange", "transportchange", "fxchange", "windowstatechange", "track-added", "track-deleted",
    "track-selected", "item-changed", "take-changed", "playback-state-changed", "tempo-changed",
    "marker-changed", "fx-changed", "project-loaded", "project-saved", "theme-changed", "native-drop", "message"};
  return names;
}
}
