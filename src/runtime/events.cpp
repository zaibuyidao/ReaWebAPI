#include "runtime/runtime.hpp"
#include <algorithm>
#include <set>

namespace reaweb {
void Runtime::emit(Session& session, const std::string& name, Json data) {
  if (session.ready && !session.closing && (name == "projectchange" || session.subscriptions.count(name)))
    session.events[name] = std::move(data);
}

void Runtime::observe(Clock::time_point deadline) {
  const auto project = host_.current_project();
  const auto generation = host_.project_generation ? host_.project_generation() : 0;
  if (project_ != project || host_generation_ != generation) {
    const bool loaded = host_generation_ != generation;
    finish_undo();
    item_index_ = 0; item_count_ = -1; item_event_ = take_event_ = transport_event_ = fx_event_ = nullptr;
    project_ = project;
    host_generation_ = generation;
    ++project_epoch_;
    project_changes_ = -1;
    selection_index_ = 0; selection_count_ = -1; last_selection_hash_ = 0;
    selection_event_ = nullptr;
    if (loaded) for (auto& item : sessions_) item.second->bridge->reset_handles();
    if (loaded) for (auto& item : sessions_) emit(*item.second, "project-loaded", {{"projectEpoch", project_epoch_}});
    next_observation_ = Clock::now();
  }
  if (Clock::now() < next_observation_) return;
  const auto changes = host_.change_count ? host_.change_count(project_) : 0;
  observe_extra(deadline, changes);
  Json next{{"projectEpoch", project_epoch_}, {"changeCount", changes}};
  if (next != project_event_) {
    project_event_ = next;
    for (auto& item : sessions_) emit(*item.second, "projectchange", next);
  }
  if (changes != project_changes_) {
    project_changes_ = changes; selection_index_ = 0; selection_count_ = -1;
    item_index_ = 0; item_count_ = -1;
  }
  bool wanted = false;
  for (const auto& item : sessions_) wanted = wanted || item.second->subscriptions.count("selectionchange") || item.second->subscriptions.count("track-selected");
  if (wanted) {
    const auto count = host_.count_selected_tracks(project_);
    if (count != selection_count_ || selection_index_ == 0) {
      selection_count_ = count; selection_index_ = 0; selection_hash_ = 14695981039346656037ull;
    }
    // Scan large selections incrementally. Events invalidate a selection, they do not copy every track.
    for (int n = 0; n < 64 && selection_index_ < count && Clock::now() < deadline; ++n, ++selection_index_) {
      auto track = host_.get_selected_track(project_, selection_index_);
      if (!track || !host_.valid_track(project_, track)) { selection_index_ = 0; selection_count_ = -1; return; }
      for (auto byte : host_.track_guid(track)) { selection_hash_ ^= byte; selection_hash_ *= 1099511628211ull; }
    }
    if (selection_index_ < count) return;
    if (last_selection_hash_ != selection_hash_ || selection_event_.is_null()) {
      last_selection_hash_ = selection_hash_;
      selection_event_ = {{"projectEpoch", project_epoch_}, {"revision", ++selection_revision_}, {"count", count}};
      for (auto& item : sessions_) { emit(*item.second, "selectionchange", selection_event_); emit(*item.second, "track-selected", selection_event_); }
    }
    selection_index_ = 0;
  }
  auto subscribed = [&](const char* name) {
    for (const auto& item : sessions_) if (item.second->subscriptions.count(name)) return true;
    return false;
  };
  if (host_.event_snapshot) for (const auto* name : {"transportchange", "fxchange"}) {
    bool transport = std::string(name) == "transportchange";
    if ((!subscribed(name) && !(transport && (subscribed("playback-state-changed") || subscribed("tempo-changed")))) || Clock::now() >= deadline) continue;
    auto state = host_.event_snapshot(name);
    if (!state.is_object()) continue;
    state["projectEpoch"] = project_epoch_;
    if (std::string(name) == "fxchange") state["changeCount"] = changes;
    auto& previous = std::string(name) == "fxchange" ? fx_event_ : transport_event_;
    if (transport) for (const auto* event : {"playback-state-changed", "tempo-changed"}) {
      const auto key = std::string(event) == "tempo-changed" ? "tempo" : "state";
      Json snapshot{{"projectEpoch", project_epoch_}, {"available", state.value("available", false)}};
      if (state.contains(key)) snapshot[key] = state[key];
      if (!extra_events_.count(event) || extra_events_[event] != snapshot) {
        extra_events_[event] = snapshot; for (auto& item : sessions_) emit(*item.second, event, snapshot);
      }
    }
    if (state != previous) { previous = state; for (auto& item : sessions_) emit(*item.second, name, state); }
  }
  if (host_.count_selected_items && host_.item_identity && (subscribed("itemselectionchange") || subscribed("takeselectionchange"))) {
    const auto count = host_.count_selected_items(project_);
    if (item_count_ != count || item_index_ == 0) {
      item_count_ = count; item_index_ = 0; take_count_ = 0;
      item_hash_ = take_hash_ = 14695981039346656037ull;
    }
    auto hash = [](uint64_t& state, const std::string& text) { for (unsigned char c : text) { state ^= c; state *= 1099511628211ull; } state ^= 255; state *= 1099511628211ull; };
    for (int n = 0; n < 64 && item_index_ < count && Clock::now() < deadline; ++n, ++item_index_) {
      const auto identity = host_.item_identity(project_, item_index_);
      if (identity.first.empty()) { item_index_ = 0; item_count_ = -1; return; }
      hash(item_hash_, identity.first); hash(take_hash_, identity.second);
      if (!identity.second.empty()) ++take_count_;
    }
    if (item_index_ < count) return;
    if (item_event_.is_null() || item_hash_ != last_item_hash_) {
      last_item_hash_ = item_hash_;
      item_event_ = {{"projectEpoch", project_epoch_}, {"revision", ++item_revision_}, {"count", count}};
      for (auto& item : sessions_) emit(*item.second, "itemselectionchange", item_event_);
    }
    if (take_event_.is_null() || take_hash_ != last_take_hash_) {
      last_take_hash_ = take_hash_;
      take_event_ = {{"projectEpoch", project_epoch_}, {"revision", ++take_revision_}, {"count", take_count_}};
      for (auto& item : sessions_) emit(*item.second, "takeselectionchange", take_event_);
    }
    item_index_ = 0;
  }
  next_observation_ = Clock::now() + std::chrono::milliseconds(100);
}
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
