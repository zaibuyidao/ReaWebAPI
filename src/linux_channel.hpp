#pragma once
#include "core.hpp"
#include <cerrno>
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace reaweb {
// Private socketpair. Neither end blocks a UI loop.
class LinuxChannel {
  int fd_;
  std::string input_;
  std::deque<std::string> output_;
  size_t offset_ = 0, queued_ = 0;
public:
  explicit LinuxChannel(int fd) : fd_(fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
      ::close(fd);
      throw std::runtime_error("Cannot configure WebKit IPC");
    }
  }
  ~LinuxChannel() { ::close(fd_); }
  void send(const Json& message) {
    auto line = message.dump(-1, ' ', true, Json::error_handler_t::replace) + '\n';
    if (line.size() > 128 * 1024 * 1024 || queued_ + line.size() > 256 * 1024 * 1024)
      throw std::runtime_error("WebKit IPC queue limit exceeded");
    queued_ += line.size();
    output_.push_back(std::move(line));
    flush();
  }
  void flush() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    for (int n = 0; n < 32 && !output_.empty() && std::chrono::steady_clock::now() < deadline; ++n) {
      const auto& line = output_.front();
      const auto count = ::send(fd_, line.data() + offset_, line.size() - offset_, MSG_NOSIGNAL);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (count <= 0) throw std::runtime_error("WebKit process disconnected");
      offset_ += static_cast<size_t>(count);
      queued_ -= static_cast<size_t>(count);
      if (offset_ == line.size()) { output_.pop_front(); offset_ = 0; }
    }
  }
  void pump(const std::function<void(const Json&)>& receive) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    flush();
    char buffer[16384];
    for (int n = 0; n < 64 && std::chrono::steady_clock::now() < deadline; ++n) {
      const auto newline = input_.find('\n');
      if (newline != std::string::npos) {
        if (newline > 128 * 1024 * 1024) throw std::runtime_error("WebKit IPC message limit exceeded");
        auto message = Json::parse(input_.substr(0, newline));
        input_.erase(0, newline + 1);
        receive(message);
        continue;
      }
      const auto count = recv(fd_, buffer, sizeof(buffer), 0);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (count <= 0) throw std::runtime_error("WebKit process disconnected");
      input_.append(buffer, static_cast<size_t>(count));
      if (input_.size() > 128 * 1024 * 1024 && input_.find('\n') == std::string::npos)
        throw std::runtime_error("WebKit IPC message limit exceeded");
    }
  }
};
}
