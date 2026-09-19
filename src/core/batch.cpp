#include "core/core.hpp"
#include "core/native.hpp"
#include "core/batch.hpp"
#include <algorithm>
#include <cmath>

namespace reaweb {
namespace {
std::string string_arg(const Json& value) {
  if (!value.is_string()) throw Error("INVALID_ARGUMENT", "Expected a string");
  auto str = value.get<std::string>();
  if (str.find('\0') != std::string::npos) throw Error("INVALID_ARGUMENT", "Embedded NUL is not allowed");
  return str;
}
double track_value(const std::string& key, const Json& input) {
  if (!input.is_number()) throw Error("INVALID_ARGUMENT", "value must be a finite number");
  const auto value = input.get<double>();
  if (!std::isfinite(value) || (key == "D_VOL" && value < 0) ||
      (key == "D_PAN" && (value < -1 || value > 1)) ||
      (key == "B_MUTE" && value != 0 && value != 1) ||
      (key == "I_SOLO" && (value < 0 || value > 2 || value != std::floor(value))) ||
      (key == "I_CUSTOMCOLOR" && (value < 0 || value > 0x1ffffff || value != std::floor(value))))
    throw Error("INVALID_ARGUMENT", "Track value is outside the supported range");
  return value;
}
}
namespace {
bool check_reference(const Json& value, size_t index) {
  if (!value.is_object() || !value.contains("$ref")) return false;
  if (value.size() > 2 || (value.size() == 2 && !value.contains("path")) ||
      !value["$ref"].is_number_integer() || value["$ref"].get<double>() < 0 || value["$ref"].get<double>() >= index)
    throw Error("INVALID_ARGUMENT", "Batch references must target an earlier result");
  if (value.contains("path")) {
    if (!value["path"].is_array() || value["path"].size() > 8) throw Error("INVALID_ARGUMENT", "Invalid reference path");
    for (const auto& part : value["path"])
      if ((!part.is_number_integer() || part.get<double>() < 0 || part.get<double>() > 1048576) && !part.is_string())
        throw Error("INVALID_ARGUMENT", "Reference paths contain property names or non-negative array indices");
  }
  return true;
}
Json resolve_references(const Json& args, const Json& results) {
  auto resolved = args;
  for (auto& arg : resolved) if (check_reference(arg, results.size())) {
    Json value = results.at(arg["$ref"].get<size_t>());
    try {
      for (const auto& part : arg.value("path", Json::array())) {
        Json next = part.is_string() ? value.at(part.get<std::string>()) : value.at(part.get<size_t>());
        value = std::move(next);
      }
    } catch (const Json::exception&) { throw Error("INVALID_ARGUMENT", "Batch reference path does not exist"); }
    arg = std::move(value);
  }
  return resolved;
}
}
void Bridge::validate_managed_call(const std::string& method, const Json& args) {
  if (!batch_methods().count(method)) throw Error("UNDO_BUSY", "This API is not supported inside a managed Undo gesture");
  const auto entry = std::find_if(native_entries().begin(), native_entries().end(), [&](const NativeEntry& e) { return method == e.name; });
  if (entry == native_entries().end()) throw Error("SCHEMA_MISMATCH", "Unknown managed Undo binding");
  observe_project();
  native_->validate_project(*entry, args, project_);
}
Json Bridge::batch(const Json& args) {
  const auto& calls = args[0];
  if (!calls.is_array() || calls.empty() || calls.size() > batch_limit)
    throw Error("INVALID_ARGUMENT", "A batch must contain 1 to 128 calls");
  std::string label;
  if (args.size() > 1) {
    if (!args[1].is_object()) throw Error("INVALID_ARGUMENT", "Expected batch options");
    for (const auto& item : args[1].items())
      if (item.key() != "undoLabel") throw Error("INVALID_ARGUMENT", "Unknown batch option: " + item.key());
    if (args[1].contains("undoLabel")) {
      label = string_arg(args[1]["undoLabel"]);
      if (label.empty() || label.size() > 256) throw Error("INVALID_ARGUMENT", "undoLabel must contain 1 to 256 UTF-8 bytes");
    }
  }
  std::vector<const NativeEntry*> entries;
  for (size_t i = 0; i < calls.size(); ++i) {
    const auto& call = calls[i];
    if (!call.is_object() || call.size() != 2 || !call.contains("method") || !call["method"].is_string() ||
        !call.contains("args") || !call["args"].is_array()) throw Error("INVALID_ARGUMENT", "Invalid batch call");
    const auto name = call["method"].get<std::string>();
    if (!batch_methods().count(name)) throw Error("INVALID_ARGUMENT", "This API is not batchable: " + name);
    const auto entry = std::find_if(native_entries().begin(), native_entries().end(), [&](const NativeEntry& e) { return name == e.name; });
    if (entry == native_entries().end()) throw Error("SCHEMA_MISMATCH", "Unknown batch binding: " + name);
    const auto& a = call["args"];
    if (a.size() < static_cast<size_t>(entry->min_args) || a.size() > static_cast<size_t>(entry->max_args))
      throw Error("INVALID_ARGUMENT", "Wrong batch argument count: " + name);
    if (!native_->resolve(entry->name)) throw Error("API_UNAVAILABLE", "Batch API unavailable: " + name);
    bool references = false;
    for (const auto& arg : a) references = check_reference(arg, i) || references;
    // Literal arguments are checked before any writes. Dependent arguments are
    // checked immediately before their call, after earlier results exist.
    native_->validate_project(*entry, a, project_);
    if (!references) native_->validate(*entry, a);
    if (name == "SetMediaTrackInfo_Value" && a[1].is_string() && a[2].is_number()) {
      const auto key = a[1].get<std::string>();
      if (key == "D_VOL" || key == "D_PAN" || key == "B_MUTE" || key == "I_SOLO" || key == "I_CUSTOMCOLOR") track_value(key, a[2]);
    }
    entries.push_back(&*entry);
  }
  if (!label.empty() && (!host_.begin_undo || !host_.end_undo)) throw Error("UNDO_UNAVAILABLE", "Undo groups are unavailable");
  Json results = Json::array();
  bool undo = false, refresh = false;
  auto finish = [&] {
    batching_ = false;
    std::exception_ptr error;
    if (refresh) { refresh = false; try { host_.prevent_refresh(-1); } catch (...) { error = std::current_exception(); } }
    if (undo) { undo = false; try { host_.end_undo(project_, label); } catch (...) { if (!error) error = std::current_exception(); } }
    if (host_.update_arrange) try { host_.update_arrange(); } catch (...) { if (!error) error = std::current_exception(); }
    if (error) std::rethrow_exception(error);
  };
  try {
    if (!label.empty()) { host_.begin_undo(project_); undo = true; }
    if (host_.prevent_refresh) { host_.prevent_refresh(1); refresh = true; }
    batching_ = true;
    for (size_t i = 0; i < calls.size(); ++i) {
      if (host_.current_project() != project_) throw Error("PROJECT_CHANGED", "The batch project changed");
      auto resolved = resolve_references(calls[i]["args"], results);
      native_->validate_project(*entries[i], resolved, project_);
      auto result = native_->invoke(*entries[i], resolved);
      // A false native return can be a legitimate query result. Preserve it,
      // except for the legacy track setter's explicit rejection contract.
      if (std::string(entries[i]->name) == "SetMediaTrackInfo_Value" && result == false)
        throw Error("NATIVE_ERROR", "REAPER rejected the track value");
      results.push_back(std::move(result));
    }
  } catch (const std::exception& e) {
    Json details{{"completed", results.size()}, {"results", results}, {"rolledBack", false}};
    if (const auto* error = dynamic_cast<const Error*>(&e)) details["cause"] = error->code;
    try { finish(); } catch (const std::exception& cleanup) { details["cleanupError"] = cleanup.what(); }
    throw Error("BATCH_FAILED", e.what(), std::move(details));
  }
  try { finish(); }
  catch (const std::exception& e) { throw Error("BATCH_FAILED", e.what(), {{"completed", results.size()}, {"results", results}, {"rolledBack", false}}); }
  return results;
}
}
