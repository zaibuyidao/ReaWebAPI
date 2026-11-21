#pragma once
#include "core/core.hpp"
#include <optional>
#include <vector>

namespace reaweb {
struct JsfxTarget {
  void* track = nullptr;
  void* take = nullptr;
  bool input = false;
  void* object() const { return take ? take : track; }
  bool operator==(const JsfxTarget& other) const { return track == other.track && take == other.take && input == other.input; }
  bool operator!=(const JsfxTarget& other) const { return !(*this == other); }
  template<class T> T api(const Host& host, const char* suffix) const {
    const auto name = std::string(take ? "TakeFX_" : "TrackFX_") + suffix;
    auto fn = host.native_function ? reinterpret_cast<T>(host.native_function(name.c_str())) : nullptr;
    if (!fn) throw Error("HOST_UNAVAILABLE", "JSFX Web UI requires " + name);
    return fn;
  }
  std::string config(const Host&, int fx, const std::string& key) const;
  std::vector<int> indices(const Host&) const;
  int selected(Host&) const;
};
class JsfxEditorHost {
public:
  virtual ~JsfxEditorHost() = default;
  virtual void* parent() const = 0;
  virtual bool valid() const = 0;
  virtual void resize(void* child) = 0;
  virtual void prepare() {}
  virtual void present(void* child) = 0;
  virtual void reveal() {}
  virtual void restore() = 0;
  virtual void park() { restore(); }
  virtual bool retain_during_switch() const { return false; }
  virtual std::optional<int> selected_fx() const { return {}; }
  virtual void sync_selection(int) {}
  virtual void read_parameter_metadata(Host&, int, Json&) const {}
  virtual void request_size(int, int, bool = false) {}
  virtual void set_background(unsigned) {}
  virtual void on_destroy(std::function<void()>) {}
};
struct JsfxEditor {
  void* track = nullptr;
  int fx = -1;
  bool floating = false;
  void* container = nullptr;
  std::shared_ptr<JsfxEditorHost> host;
  void* take = nullptr;
  bool input = false;
  JsfxTarget target() const { return {track, take, input}; }
};
class JsfxEditorDiscovery {
public:
  struct Size { int width, height; bool exact = false; };
  using Match = std::function<std::optional<Size>(JsfxTarget, int)>;
  virtual ~JsfxEditorDiscovery() = default;
  virtual std::vector<JsfxEditor> scan(Host&, Match) = 0;
  virtual bool pending() const { return false; }
  virtual Json diagnostics() const { return Json::object(); }
};
using JsfxDiscovery = std::shared_ptr<JsfxEditorDiscovery>;
JsfxDiscovery make_jsfx_editor_discovery();
}
