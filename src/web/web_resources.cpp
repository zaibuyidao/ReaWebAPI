#include <httplib.h>
#include "web/web_resources.hpp"
#include "core/worker.hpp"
#include <fstream>

namespace reaweb {
namespace {
std::string root_identity(const fs::path& root) {
  auto value = fs::canonical(root).generic_u8string();
#ifdef _WIN32
  // Windows paths are case insensitive. Keep ordinary launcher path casing
  // differences from creating a second browser profile.
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c);
  });
#endif
  return value;
}
std::string encode_path(const std::string& path) {
  const char* hex = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : path) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~' || c == '/') result += static_cast<char>(c);
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}
bool resource_within_root(const fs::path& root, const std::string& url_path) {
  if (url_path.empty() || url_path.front() != '/') return false;
  auto relative = fs::u8path(url_path.substr(1));
  if (relative.has_root_path()) return false;
  // Match the static mount's implicit index so /pages/ cannot bypass the check
  // through an index.html link, even when the directory itself is inside root.
  if (url_path.back() == '/') relative /= "index.html";
  std::error_code error;
  const auto resolved = fs::weakly_canonical(root / relative, error);
  if (error) return false;
  const auto contained = resolved.lexically_relative(root);
  return !contained.empty() && !contained.is_absolute() && *contained.begin() != "..";
}
}
std::string app_identity(const fs::path& root) { return state_key(root_identity(root), 0); }
struct WebResources::Impl {
  fs::path root;
  std::string origin, authority;
  httplib::Server server;
  std::thread thread;
  ~Impl() {
    server.stop();
    if (thread.joinable()) thread.join();
  }
};
WebResources::WebResources(const fs::path& root, const fs::path& profile) : impl_(std::make_unique<Impl>()) {
  auto& p = *impl_;
  p.root = fs::canonical(root);
  fs::create_directories(profile);
  const auto identity = root_identity(p.root);
  const auto record = profile / "origin.json";
  int port = 0;
  if (fs::exists(record)) {
    try {
      if (fs::file_size(record) > 65536) throw std::runtime_error("Origin record exceeds limit");
      std::ifstream input(record);
      auto data = Json::parse(input);
      port = data.at("port").get<int>();
      if (data.at("schema") != 1 || data.at("root") != identity || port < 1024 || port > 65535)
        throw std::runtime_error("Origin record does not match this App");
    } catch (const std::exception& e) {
      throw Error("APP_ORIGIN_INVALID", std::string("Cannot restore App origin: ") + e.what());
    }
  }
  // Fixed small pool and bounded connection queue; browser primitives run in
  // the WebView and REAPER calls continue through the existing main-thread bridge.
  p.server.new_task_queue = [] { return new httplib::ThreadPool(2, 2, 32); };
  p.server.set_read_timeout(2);
  p.server.set_write_timeout(2);
  p.server.set_keep_alive_max_count(1);
  p.server.set_keep_alive_timeout(1);
  p.server.set_payload_max_length(0);
#ifdef _WIN32
  p.server.set_socket_options([](socket_t socket) {
    BOOL exclusive = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
  });
#else
  // Allow restart after TIME_WAIT, but never SO_REUSEPORT: another process must
  // not take a share of the same persistent App origin's incoming requests.
  p.server.set_socket_options([](socket_t socket) {
    const int reuse = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  });
#endif
  p.server.set_default_headers({
    {"Cache-Control", "no-cache"},
    {"X-Content-Type-Options", "nosniff"},
    {"Cross-Origin-Resource-Policy", "same-origin"},
    {"Content-Security-Policy", "frame-ancestors 'none'"},
    {"X-Frame-Options", "DENY"}
  });
  p.server.set_pre_routing_handler([&p](const httplib::Request& request, httplib::Response& response) {
    if (request.get_header_value_count("Host") != 1 || request.get_header_value("Host") != p.authority ||
        (request.has_header("Origin") && request.get_header_value("Origin") != p.origin) ||
        (request.has_header("Sec-Fetch-Site") && request.get_header_value("Sec-Fetch-Site") != "same-origin" &&
         request.get_header_value("Sec-Fetch-Site") != "none")) {
      response.status = 403;
    } else if (request.method != "GET" && request.method != "HEAD") {
      response.status = 405;
      response.set_header("Allow", "GET, HEAD");
    } else if (request.path.find_first_of("\\:") != std::string::npos ||
               std::any_of(request.path.begin(), request.path.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
      response.status = 400;
    } else if (!resource_within_root(p.root, request.path)) {
      response.status = 403;
    } else return httplib::Server::HandlerResponse::Unhandled;
    response.set_content("App resource request rejected", "text/plain; charset=utf-8");
    return httplib::Server::HandlerResponse::Handled;
  });
  // Our guard resolves real filesystem targets on every platform. The pinned
  // library's Windows _fullpath check is lexical and does not resolve links.
  // Keep its static MIME/range/conditional handling and disabled directory listing.
  if (!p.server.set_mount_point("/", p.root.u8string()))
    throw Error("APP_RESOURCE_ERROR", "Cannot mount the App directory");
  for (const auto* extension : {"js", "mjs"})
    p.server.set_file_extension_and_mimetype_mapping(extension, "text/javascript; charset=utf-8");
  p.server.set_file_extension_and_mimetype_mapping("json", "application/json; charset=utf-8");
  p.server.set_file_extension_and_mimetype_mapping("wasm", "application/wasm");
  if (port) {
    if (!p.server.bind_to_port("127.0.0.1", port))
      throw Error("APP_ORIGIN_BUSY", "This App's saved loopback port " + std::to_string(port) +
        " is occupied. Close the conflicting process and reopen; the origin is kept to preserve browser storage.");
  } else {
    port = p.server.bind_to_any_port("127.0.0.1");
    if (port < 1024) throw Error("APP_RESOURCE_ERROR", "Cannot allocate a loopback App origin");
    auto temporary = record; temporary += ".tmp";
    try {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      output << Json{{"schema", 1}, {"root", identity}, {"port", port}}.dump(2);
      output.close();
      if (!output) throw std::runtime_error("Cannot persist the App origin");
      fs::rename(temporary, record);
    } catch (...) { std::error_code ignored; fs::remove(temporary, ignored); throw; }
  }
#ifdef __APPLE__
  // macOS 14+ ATS restricts IP-literal HTTP URLs. The unqualified localhost
  // name uses the local-load allowance without changing REAPER's Info.plist.
  p.authority = "localhost:" + std::to_string(port);
#else
  p.authority = "127.0.0.1:" + std::to_string(port);
#endif
  p.origin = "http://" + p.authority;
  p.thread = std::thread([&p] { p.server.listen_after_bind(); });
  p.server.wait_until_ready();
  if (!p.server.is_running()) throw Error("APP_RESOURCE_ERROR", "The App resource listener could not start");
}
WebResources::~WebResources() = default;
const std::string& WebResources::origin() const { return impl_->origin; }
std::string WebResources::entry_url(const fs::path& entry) const {
  const auto relative = fs::canonical(entry).lexically_relative(impl_->root);
  if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
    throw Error("INVALID_PATH", "The entry must be inside the App directory");
  return impl_->origin + "/" + encode_path(relative.generic_u8string());
}
}
