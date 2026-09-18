#pragma once
#include "core.hpp"
#include <memory>

namespace reaweb {
struct WindowOptions {
  fs::path entry;
  std::string script;
  std::string title;
  std::function<void(std::string)> on_message;
  std::function<void(std::string)> on_error;
};
class Window {
public:
  virtual ~Window() = default;
  virtual void evaluate(const std::string& script) = 0;
  virtual void devtools() = 0;
  virtual bool closed() const = 0;
};
class Platform {
public:
  virtual ~Platform() = default;
  virtual std::shared_ptr<Window> open(WindowOptions options) = 0;
  virtual void pump() {}
};
std::unique_ptr<Platform> make_platform(const fs::path& shared_data);
}
