#include "runtime.hpp"
#include <algorithm>
#include <set>

namespace reaweb {
void Runtime::observe_extra(Clock::time_point deadline, int changes) {
  auto wanted = [&](const char* name) {
    for (const auto& item : sessions_) if (item.second->subscriptions.count(name)) return true;
    return false;
  };
  auto publish = [&](const std::string& name, Json data) {
    extra_events_[name] = data;
    for (auto& item : sessions_) emit(*item.second, name, data);
  };
  if (extra_epoch_ != project_epoch_) {
    extra_epoch_ = project_epoch_; extra_change_ = -1;
    track_scan_.clear(); last_tracks_.clear(); track_scan_count_ = -1; tracks_initialized_ = false;
    extra_events_.clear(); extra_revisions_.clear(); saved_project_state_ = nullptr;
  }
  const bool changed = extra_change_ != changes;
  extra_change_ = changes;
  if (changed) track_scan_.clear();
  // Content notifications deliberately invalidate project-wide caches. They
  // are not an edit journal and do not fabricate an affected Item/Take.
  for (const char* name : {"item-changed", "take-changed"}) if (wanted(name) && (changed || !extra_events_.count(name)))
    publish(name, {{"projectEpoch", project_epoch_}, {"changeCount", changes}, {"scope", "project"}, {"invalidated", true}});
  if ((wanted("track-added") || wanted("track-deleted")) && host_.track_count && host_.track_identity) {
    int count = host_.track_count();
    const bool count_changed = count != track_scan_count_;
    if (count_changed) { track_scan_count_ = count; track_scan_.clear(); }
    if (!tracks_initialized_ || changed || count_changed || !track_scan_.empty()) {
      for (int n = 0; n < 64 && static_cast<int>(track_scan_.size()) < count && Clock::now() < deadline; ++n) {
        auto id = host_.track_identity(static_cast<int>(track_scan_.size()));
        if (id.empty()) { track_scan_.clear(); return; }
        track_scan_.push_back(std::move(id));
      }
      if (static_cast<int>(track_scan_.size()) == count) {
        if (tracks_initialized_ && last_tracks_ != track_scan_) {
          const std::set<std::string> previous(last_tracks_.begin(), last_tracks_.end()), current(track_scan_.begin(), track_scan_.end());
          Json added = Json::array(), removed = Json::array();
          for (const auto& id : current) if (!previous.count(id)) added.push_back(id);
          for (const auto& id : previous) if (!current.count(id)) removed.push_back(id);
          ++track_revision_;
          // Transition events have no initial snapshot or historical replay.
          for (auto& item : sessions_) {
            if (!added.empty()) emit(*item.second, "track-added", {{"projectEpoch", project_epoch_}, {"revision", track_revision_}, {"guids", added}});
            if (!removed.empty()) emit(*item.second, "track-deleted", {{"projectEpoch", project_epoch_}, {"revision", track_revision_}, {"guids", removed}});
          }
        }
        last_tracks_ = std::move(track_scan_); track_scan_.clear(); tracks_initialized_ = true;
      }
    }
  } else { track_scan_.clear(); track_scan_count_ = -1; tracks_initialized_ = false; }
  if (host_.event_revision) for (const char* name : {"marker-changed", "fx-changed"}) if (wanted(name)) {
    const auto revision = host_.event_revision(name);
    auto previous = extra_revisions_.find(name);
    if (previous == extra_revisions_.end() || previous->second != revision) {
      extra_revisions_[name] = revision;
      publish(name, {{"projectEpoch", project_epoch_}, {"revision", revision}, {"invalidated", true}});
    }
  }
  if (host_.project_save_state && wanted("project-saved") && Clock::now() < deadline) {
    const auto state = host_.project_save_state();
    if (state.is_object() && state.contains("serialization")) {
      if (saved_project_state_.is_object() && state.value("available", false) &&
          (state["path"] != saved_project_state_["path"] || state["stamp"] != saved_project_state_["stamp"]) &&
          state["serialization"] != saved_project_state_["serialization"] && !state.value("dirty", true)) {
        for (auto& item : sessions_) emit(*item.second, "project-saved", {{"projectEpoch", project_epoch_},
          {"path", state["path"]}, {"observation", "file-updated-and-project-clean"}});
      }
      // Keep the pre-save stamp during serialization / failed or unfinished I/O.
      if (saved_project_state_.is_null() || !state.value("dirty", true))
        saved_project_state_ = state;
    }
  }
  if (wanted("theme-changed") && Clock::now() >= next_theme_ && Clock::now() < deadline) {
    next_theme_ = Clock::now() + std::chrono::milliseconds(500);
    auto theme = theme_colors(host_);
    if (!extra_events_.count("theme-changed") || extra_events_["theme-changed"] != theme) publish("theme-changed", std::move(theme));
  }
}
}
