#pragma once
#include "platform/shared/jsfx_editor.hpp"
#include <optional>

namespace reaweb {
struct JsfxAssociation {
  fs::path entry;
  std::string ident;
  int width = 640, height = 420;
  Json native;
  bool daw_keyboard = true;
};
std::optional<JsfxAssociation> jsfx_association(const fs::path& effects, const std::string& ident);
std::string jsfx_display_name(const fs::path& source);

class JsfxBinding {
  Host& host_;
  void* project_ = nullptr;
  JsfxTarget target_;
  void* item_ = nullptr;
  Guid track_guid_{}, item_guid_{}, take_guid_{}, fx_guid_{};
  uint64_t generation_ = 0;
  int index_ = -1;
  std::string ident_;
  void validate_owner();
public:
  JsfxBinding(Host& host, void* track, int fx);
  JsfxBinding(Host& host, JsfxTarget target, int fx);
  int resolve();
  bool editor_matches(const JsfxEditor& editor);
  std::string ident();
  Json state();
  Json parameter(int param, bool metadata = false);
  int parameter_count();
  void set(int param, double value, bool normalized);
  void end(int param);
  void* project() const { return project_; }
  void* track() const { return target_.track; }
  JsfxTarget target() const { return target_; }
  const Guid& guid() const { return fx_guid_; }
};
}
