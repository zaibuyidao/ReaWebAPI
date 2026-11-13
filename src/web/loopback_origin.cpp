#include "web/loopback_origin.hpp"
#include "runtime/external_settings.hpp"
#include <httplib.h>
#include <fstream>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace reaweb {
namespace {
bool equal_secret(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  unsigned difference = 0;
  for (size_t i = 0; i < left.size(); ++i) difference |= static_cast<unsigned char>(left[i] ^ right[i]);
  return difference == 0;
}
bool has_cookie(const std::string& cookies, const std::string& expected) {
  size_t begin = 0;
  while (begin < cookies.size()) {
    while (begin < cookies.size() && cookies[begin] == ' ') ++begin;
    auto end = cookies.find(';', begin);
    if (end == std::string::npos) end = cookies.size();
    if (equal_secret(cookies.substr(begin, end - begin), expected)) return true;
    begin = end + 1;
  }
  return false;
}
struct RecordLock {
  int fd;
  explicit RecordLock(const fs::path& path) : fd(::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)) {
    if (fd < 0) throw Error("APP_DATA_UNAVAILABLE", "Cannot open isolated-origin lock");
    if (flock(fd, LOCK_EX) != 0) { ::close(fd); throw Error("APP_DATA_UNAVAILABLE", "Cannot lock isolated origin"); }
  }
  ~RecordLock() { flock(fd, LOCK_UN); ::close(fd); }
};
}
struct LoopbackOrigin::Impl {
  httplib::Server server;
  std::thread listener;
  std::shared_ptr<WebResources> primary;
  std::mutex mutex;
  std::vector<std::weak_ptr<WebResources>> windows;
  std::string origin, authority, authorization, cookie;
  Impl(const fs::path& profile, const std::string& app_id, std::shared_ptr<WebResources> resources) : primary(std::move(resources)) {
    server.set_socket_options([](socket_t socket) {
      const int enabled = 1;
      setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    });
    fs::create_directories(profile);
    RecordLock lock(profile / "isolation-origin.lock");
    const auto record = profile / "isolation-origin.json";
    int port = 0;
    if (fs::exists(record)) {
      try {
        if (!fs::is_regular_file(record) || fs::file_size(record) > 4096) throw std::runtime_error("invalid record");
        std::ifstream input(record); const auto value = Json::parse(input);
        port = value.at("port").get<int>();
        if (value.at("schema") != 1 || value.at("appId") != app_id || port < 10240 || port > 49151)
          throw std::runtime_error("invalid mapping");
      } catch (const std::exception&) { throw Error("APP_ORIGIN_INVALID", "Invalid isolation-origin.json"); }
      if (!server.bind_to_port("127.0.0.1", port))
        throw Error("APP_ORIGIN_IN_USE", "The persistent isolated App port is already in use: " + std::to_string(port));
    } else {
      uint32_t hash = 2166136261u;
      for (const unsigned char c : fs::canonical(profile).u8string() + ":" + app_id) hash = (hash ^ c) * 16777619u;
      for (int attempt = 0; attempt < 38912; ++attempt) {
        const int candidate = 10240 + ((hash + static_cast<uint32_t>(attempt)) % 38912);
        if (server.bind_to_port("127.0.0.1", candidate)) { port = candidate; break; }
      }
      if (!port) throw Error("APP_ORIGIN_IN_USE", "No isolated App port is available");
      const auto temporary = profile / ("isolation-origin." + external_token() + ".tmp");
      std::ofstream output(temporary);
      output << Json{{"schema", 1}, {"appId", app_id}, {"port", port}, {"previousOrigin", "reaweb://" + app_id}}.dump(2) << '\n';
      output.close();
      if (!output) throw Error("APP_DATA_UNAVAILABLE", "Cannot persist isolated App origin");
      fs::rename(temporary, record);
    }
    authority = "127.0.0.1:" + std::to_string(port);
    origin = "http://" + authority;
    const auto token = external_token();
    authorization = "Bearer " + token;
    cookie = "reaweb_isolation_" + std::to_string(port) + "=" + token;
    primary->configure_loopback(origin, authorization);
    server.set_payload_max_length(16384);
    server.set_read_timeout(2, 0); server.set_write_timeout(2, 0); server.set_keep_alive_max_count(20);
    server.set_pre_routing_handler([this](const httplib::Request& request, httplib::Response& response) {
      if (request.get_header_value("Host") != authority ||
          (request.has_header("Origin") && request.get_header_value("Origin") != origin) ||
          (!equal_secret(request.get_header_value("Authorization"), authorization) && !has_cookie(request.get_header_value("Cookie"), cookie))) {
        response.status = 403; response.set_header("Cache-Control", "no-store"); return httplib::Server::HandlerResponse::Handled;
      }
      if (equal_secret(request.get_header_value("Authorization"), authorization))
        response.set_header("Set-Cookie", cookie + "; HttpOnly; SameSite=Strict; Path=/");
      if (request.method != "GET" && request.method != "HEAD") {
        response.status = 405; return httplib::Server::HandlerResponse::Handled;
      }
      return httplib::Server::HandlerResponse::Unhandled;
    });
    server.Get("/.*", [this](const httplib::Request& request, httplib::Response& response) {
      std::map<std::string, std::string> headers;
      for (const auto* key : {"origin", "sec-fetch-site", "range", "if-range", "if-none-match", "if-modified-since"})
        if (request.has_header(key)) headers[key] = request.get_header_value(key);
      auto provider = primary;
      auto result = provider->request(origin + request.target, request.method, headers);
      if (result.status == 404 && request.path.rfind("/__reaweb_media__/", 0) == 0) {
        std::lock_guard<std::mutex> guard(mutex);
        for (auto it = windows.begin(); it != windows.end();) {
          if (auto resources = it->lock()) {
            provider = resources;
            result = resources->request(origin + request.target, request.method, headers);
            if (result.status != 404) break;
            ++it;
          } else it = windows.erase(it);
        }
      }
      const bool partial = result.status == 206;
      if (partial) {
        // cpp-httplib applies the validated range to the full content provider.
        headers.erase("range");
        result = provider->request(origin + request.target, request.method, headers);
      }
      response.status = partial && result.status == 200 ? 206 : result.status;
      const bool content_provider = result.size && (result.data || result.reader);
      for (const auto& [name, value] : result.headers)
        if ((name != "Content-Length" || request.method == "HEAD") && (name != "Content-Type" || !content_provider))
          response.set_header(name, value);
      if (content_provider) {
        const auto type = result.headers.at("Content-Type");
        response.set_content_provider(result.size, type, [result = std::move(result)](size_t offset, size_t length, httplib::DataSink& sink) {
          if (result.reader) {
            std::vector<char> bytes(std::min<size_t>(length, 65536));
            const auto count = result.reader->read(result.offset + offset, bytes.data(), bytes.size());
            return count && sink.write(bytes.data(), count);
          }
          return sink.write(result.data + offset, length);
        });
      }
    });
    listener = std::thread([this] { server.listen_after_bind(); });
  }
  ~Impl() { server.stop(); if (listener.joinable()) listener.join(); }
};
LoopbackOrigin::LoopbackOrigin(const fs::path& profile, const std::string& app_id, std::shared_ptr<WebResources> resources)
  : impl_(std::make_unique<Impl>(profile, app_id, std::move(resources))) {}
LoopbackOrigin::~LoopbackOrigin() = default;
void LoopbackOrigin::attach(const std::shared_ptr<WebResources>& resources) {
  resources->configure_loopback(impl_->origin, impl_->authorization);
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->windows.push_back(resources);
}
}
