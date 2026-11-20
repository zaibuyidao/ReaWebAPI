#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "runtime/external_transport.hpp"
#include <sha.h>
#include <wdl_base64.h>
#include <algorithm>
#include <cerrno>
#include <sstream>
#include <thread>

namespace reaweb {
namespace {
using Time = std::chrono::steady_clock;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
bool blocked() { return WSAGetLastError() == WSAEWOULDBLOCK; }
void nonblocking(Socket socket) { u_long on = 1; ioctlsocket(socket, FIONBIO, &on); }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket socket) { ::close(socket); }
bool blocked() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
void nonblocking(Socket socket) { fcntl(socket, F_SETFL, fcntl(socket, F_GETFL) | O_NONBLOCK); }
#endif
size_t json_cost(const Json& value) {
  size_t size = 64;
  if (value.is_string()) return size + value.get_ref<const std::string&>().size() * 6;
  if (value.is_structured()) for (const auto& item : value.items()) {
    size += (value.is_object() ? item.key().size() * 6 : 0) + json_cost(item.value());
    if (size > message_limit) break;
  }
  return size;
}
std::string frame(const std::string& data, unsigned opcode = 1) {
  std::string result(1, char(128 | opcode));
  if (data.size() < 126) result += char(data.size());
  else if (data.size() <= 65535) { result += char(126); result += char(data.size() >> 8); result += char(data.size()); }
  else { result += char(127); for (int i = 7; i >= 0; --i) result += char(uint64_t(data.size()) >> (8 * i)); }
  result += data;
  return result;
}
std::string lower(std::string text) { for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return text; }
std::string trim(const std::string& text) {
  const auto a = text.find_first_not_of(" \t\r"), b = text.find_last_not_of(" \t\r");
  return a == std::string::npos ? "" : text.substr(a, b - a + 1);
}
}
bool ExternalConnection::take(Request& request, const std::function<bool(const Json&)>& input) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!alive || closing || requests_.empty()) return false;
  auto next = requests_.begin();
  if (input) for (auto it = requests_.begin(); it != requests_.end(); ++it)
    if (it->authenticated && it->bytes <= 4096 && input(it->data)) { next = it; break; }
  request = std::move(*next); requests_.erase(next); input_bytes_ -= request.bytes;
  return true;
}
bool ExternalConnection::send(Json message, bool finish) {
  auto bytes = json_cost(message);
  if (bytes > message_limit) {
    if (message.value("type", "") != "response") { close(); return false; }
    message = {{"type", "response"}, {"id", message.at("id")},
      {"error", {{"code", "RESPONSE_LIMIT"}, {"message", "Response exceeds the control channel limit"}}}};
    bytes = json_cost(message);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!alive || closing) return false;
  if (responses_.size() >= 256 || output_bytes_ + bytes > message_limit) { alive = false; return false; }
  responses_.emplace_back(std::move(message), bytes); output_bytes_ += bytes;
  if (finish) closing = true;
  return true;
}
void ExternalConnection::close() { alive = false; }
bool ExternalConnection::receive(const std::string& text) {
  Json data;
  try {
    data = Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
      if (depth > 64) throw Error("INVALID_REQUEST", "JSON nesting exceeds 64 levels");
      return true;
    });
    if (!data.is_object() || !data.contains("id") || !data["id"].is_number_integer() ||
        data["id"].get<double>() < 1 || data["id"].get<double>() > 9007199254740991.0) return false;
  } catch (...) { return false; }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!alive || closing || pending_.size() >= ExternalTransport::pending_limit ||
      input_bytes_ + text.size() > message_limit || !pending_.insert(data["id"].get<uint64_t>()).second) return false;
  requests_.push_back({std::move(data), Time::now(), text.size(), authenticated.load()}); input_bytes_ += text.size();
  return true;
}
std::string ExternalConnection::output() {
  Json data;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (responses_.empty()) return {};
    auto next = std::move(responses_.front()); responses_.pop_front(); output_bytes_ -= next.second;
    data = std::move(next.first);
    if (data.value("type", "") == "response") pending_.erase(data.at("id").get<uint64_t>());
  }
  return frame(data.dump(-1, ' ', true, Json::error_handler_t::replace));
}

struct ExternalTransport::Impl {
  struct Client {
    Socket socket;
    std::shared_ptr<ExternalConnection> channel = std::make_shared<ExternalConnection>();
    std::string input, fragments, output;
    size_t offset = 0;
    bool upgraded = false, fragmented = false, finish = false;
    Time::time_point opened = Time::now(), progress = Time::now(), message_started = Time::now();
  };
  Socket listener = invalid_socket;
  uint16_t port;
  std::atomic<bool> stopping{false};
  std::thread thread;
  std::mutex mutex;
  std::vector<std::shared_ptr<ExternalConnection>> channels;
  std::vector<Client> clients;
  explicit Impl(uint16_t requested) : port(requested) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) throw Error("TRANSPORT_UNAVAILABLE", "Cannot initialize local sockets");
#endif
    try {
      listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
      int exclusive = 1;
      if (listener != invalid_socket) setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
      int reuse = 1;
      if (listener != invalid_socket) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
      sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (listener == invalid_socket || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, 16))
        throw Error("TRANSPORT_UNAVAILABLE", "Cannot bind External Client server to 127.0.0.1:" + std::to_string(port));
#ifdef _WIN32
      int size = sizeof(address);
#else
      socklen_t size = sizeof(address);
#endif
      getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size); port = ntohs(address.sin_port);
      nonblocking(listener);
      thread = std::thread([this] { run(); });
    } catch (...) {
      if (listener != invalid_socket) close_socket(listener);
#ifdef _WIN32
      WSACleanup();
#endif
      throw;
    }
  }
  ~Impl() {
    stopping = true;
    if (thread.joinable()) thread.join();
    for (auto& client : clients) { client.channel->close(); close_socket(client.socket); }
    close_socket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
  }
  bool handshake(Client& client) {
    const auto end = client.input.find("\r\n\r\n");
    if (end == std::string::npos) return client.input.size() <= 8192;
    if (end > 8192) return false;
    std::istringstream input(client.input.substr(0, end)); std::string line, method, path, version;
    std::getline(input, line); std::istringstream first(line); first >> method >> path >> version;
    if (method != "GET" || path != "/" || version != "HTTP/1.1") return false;
    std::map<std::string, std::string> headers;
    while (std::getline(input, line)) {
      auto colon = line.find(':');
      if (colon == std::string::npos || !headers.emplace(lower(line.substr(0, colon)), trim(line.substr(colon + 1))).second) return false;
    }
    if (headers["host"] != "127.0.0.1:" + std::to_string(port) || lower(headers["upgrade"]) != "websocket" ||
        lower(headers["connection"]).find("upgrade") == std::string::npos || headers["sec-websocket-version"] != "13" ||
        headers["sec-websocket-key"].size() != 24) return false;
    const auto key = headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[20]; char accept[32]; WDL_SHA1 sha; sha.add(key.data(), int(key.size())); sha.result(digest);
    wdl_base64encode(digest, accept, 20);
    client.output = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ") + accept + "\r\n\r\n";
    client.upgraded = true; client.input.erase(0, end + 4);
    return true;
  }
  bool incoming(Client& client) {
    for (int n = 0; n < 16 && client.input.size() >= 2; ++n) {
      const auto* data = reinterpret_cast<const unsigned char*>(client.input.data());
      const unsigned opcode = data[0] & 15;
      const bool fin = (data[0] & 128) != 0;
      if ((data[0] & 112) || !(data[1] & 128)) return false;
      uint64_t length = data[1] & 127; size_t header = 2;
      if (length == 126) {
        if (client.input.size() < 4) return true;
        length = uint64_t(data[2]) * 256 + data[3]; header = 4;
        if (length < 126) return false;
      } else if (length == 127) {
        if (client.input.size() < 10) return true;
        length = 0; for (int i = 2; i < 10; ++i) length = (length << 8) | data[i]; header = 10;
        if (length <= 65535) return false;
      }
      if (length > message_limit || (opcode >= 8 && (!fin || length > 125))) return false;
      if (client.input.size() < header + 4 + length) return true;
      std::string payload(size_t(length), '\0');
      for (size_t i = 0; i < length; ++i) payload[i] = char(data[header + 4 + i] ^ data[header + i % 4]);
      client.input.erase(0, header + 4 + size_t(length));
      if (opcode == 8) return false;
      if (opcode == 9) {
        if (client.output.size() > message_limit) return false;
        client.output += frame(payload, 10);
        continue;
      }
      if (opcode == 10) continue;
      if ((opcode != 1 && opcode != 0) || (opcode == 0) != client.fragmented) return false;
      if (client.fragments.size() + payload.size() > message_limit) return false;
      if (!client.fragmented) client.message_started = Time::now();
      client.fragments += payload; client.fragmented = !fin;
      if (fin) {
        if (!client.channel->receive(client.fragments)) return false;
        client.fragments.clear();
      }
    }
    return true;
  }
  bool pump(Client& client) {
    if (!client.channel->alive) return false;
    if (!client.channel->authenticated && Time::now() - client.opened > std::chrono::seconds(5)) return false;
    if (!client.channel->closing) {
      char bytes[65536]; const auto read = recv(client.socket, bytes, sizeof(bytes), 0);
      if (!read || (read < 0 && !blocked())) return false;
      if (read > 0) {
        if (client.input.empty() && !client.fragmented) client.message_started = Time::now();
        client.input.append(bytes, size_t(read));
      }
      if (client.input.size() + client.fragments.size() > message_limit + 14 ||
          (!client.upgraded && !handshake(client)) || (client.upgraded && !incoming(client))) return false;
      if ((!client.input.empty() || client.fragmented) && Time::now() - client.message_started > std::chrono::seconds(30)) return false;
    }
    if (client.output.empty() && client.upgraded) {
      client.output = client.channel->output();
      if (client.output.empty() && client.channel->closing) {
        client.output = frame(std::string("\x03\xe8", 2), 8); client.finish = true;
      }
    }
    if (!client.output.empty()) {
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      auto written = ::send(client.socket, client.output.data() + client.offset,
        int(std::min<size_t>(65536, client.output.size() - client.offset)), flags);
      if (written < 0 && !blocked()) return false;
      if (written > 0) { client.offset += size_t(written); client.progress = Time::now(); }
      if (client.offset == client.output.size()) {
        client.output.clear(); client.offset = 0;
        if (client.finish) return false;
      }
      if (Time::now() - client.progress > std::chrono::seconds(30)) return false;
    } else client.progress = Time::now();
    return true;
  }
  void run() noexcept {
    try {
      while (!stopping) {
        for (int n = 0; n < 4; ++n) {
          auto connection = accept(listener, nullptr, nullptr);
          if (connection == invalid_socket) break;
          if (clients.size() >= connection_limit) { close_socket(connection); continue; }
          nonblocking(connection); int on = 1;
          setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
#ifdef SO_NOSIGPIPE
          setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
          clients.push_back(Client{connection});
          std::lock_guard<std::mutex> lock(mutex);
          channels.push_back(clients.back().channel);
        }
        for (auto it = clients.begin(); it != clients.end();) {
          if (pump(*it)) { ++it; continue; }
          it->channel->close(); close_socket(it->socket); it = clients.erase(it);
        }
        { std::lock_guard<std::mutex> lock(mutex);
          channels.erase(std::remove_if(channels.begin(), channels.end(), [](const auto& channel) { return !channel->alive; }), channels.end()); }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } catch (...) { for (auto& client : clients) client.channel->close(); }
  }
};
ExternalTransport::ExternalTransport(uint16_t port) : impl_(std::make_unique<Impl>(port)) {}
ExternalTransport::~ExternalTransport() = default;
uint16_t ExternalTransport::port() const { return impl_->port; }
std::vector<std::shared_ptr<ExternalConnection>> ExternalTransport::connections() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->channels;
}
}
