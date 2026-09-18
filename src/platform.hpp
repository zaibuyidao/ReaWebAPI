#pragma once
#include "core.hpp"
#include <memory>

namespace reaweb {
struct DockApi {
  void* parent = nullptr;
  std::function<void(void*, const std::string&, const std::string&)> add;
  std::function<void(void*)> remove;
  std::function<int(void*)> index;
  std::function<void(void*)> activate;
  std::function<void(const std::string&, int)> remember;
  std::function<void(void*)> refresh;
};
struct WindowOptions {
  fs::path entry;
  std::string script;
  std::string title;
  std::function<void(std::string)> on_message;
  std::function<void(std::string)> on_error;
  void* parent = nullptr;
  std::function<void()> on_navigation;
};
class Window {
public:
  virtual ~Window() = default;
  virtual void evaluate(const std::string& script) = 0;
  virtual void devtools() = 0;
  virtual bool closed() const = 0;
  virtual void* native_handle() const { return nullptr; }
  virtual void prepare_dock() {}
  virtual void prepare_undock() {}
  virtual void restore_floating() {}
  virtual void tick() {}
  virtual void focus() {}
  virtual void set_title(const std::string&) {}
  virtual bool visible() const { return !closed(); }
  virtual bool focused() const { return false; }
  virtual Json placement() const { return nullptr; }
  virtual void restore_placement(const Json&) {}
  virtual Json diagnostics() const { return {{"backend", "unknown"}}; }
};
class Platform {
public:
  virtual ~Platform() = default;
  virtual std::shared_ptr<Window> open(WindowOptions options) = 0;
  virtual void pump() {}
};
std::unique_ptr<Platform> make_platform(const fs::path& shared_data);
}
