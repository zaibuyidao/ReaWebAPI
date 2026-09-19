#include "runtime/runtime.hpp"

namespace reaweb {
void Runtime::finish_undo() {
  if (!undo_owner_) return;
  const auto project = undo_project_;
  const auto label = undo_label_;
  undo_owner_ = 0; undo_project_ = nullptr; undo_token_.clear(); undo_label_.clear();
  using Validate = bool (*)(void*, void*, const char*);
  auto validate = host_.native_function ? reinterpret_cast<Validate>(host_.native_function("ValidatePtr2")) : nullptr;
  if ((!validate || validate(nullptr, project, "ReaProject*")) && host_.end_undo) host_.end_undo(project, label);
  if (host_.update_arrange) host_.update_arrange();
}

Json Runtime::transaction_call(int id, const std::string& method, const Json& args) {
  if (method == "ReaWeb_BeginUndo") {
    if (undo_owner_) throw Error("UNDO_BUSY", "Another managed Undo gesture is active");
    if (!args[0].is_string()) throw Error("INVALID_ARGUMENT", "Expected an Undo label");
    auto label = args[0].get<std::string>();
    if (label.empty() || label.size() > 256 || label.find('\0') != std::string::npos) throw Error("INVALID_ARGUMENT", "Invalid Undo label");
    if (!host_.begin_undo || !host_.end_undo) throw Error("UNDO_UNAVAILABLE", "Undo groups are unavailable");
    auto project = host_.current_project();
    host_.begin_undo(project);
    undo_owner_ = id; undo_project_ = project; undo_label_ = std::move(label);
    undo_token_ = std::to_string(id) + ":" + std::to_string(++undo_sequence_);
    undo_deadline_ = Clock::now() + std::chrono::seconds(30);
    return undo_token_;
  }
  if (method == "ReaWeb_EndUndo") {
    if (!args[0].is_string() || undo_owner_ != id || args[0] != undo_token_) throw Error("STALE_UNDO", "The Undo gesture already ended or belongs to another page");
    finish_undo(); return true;
  }
  throw Error("UNKNOWN_API", "Unknown transaction operation");
}
}
