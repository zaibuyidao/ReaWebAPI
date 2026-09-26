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
#include "runtime/stream_transport.hpp"
#include "runtime/native_stream.hpp"
#include <sha.h>
#include <wdl_base64.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <sstream>

namespace reaweb {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void socket_close(Socket socket) { closesocket(socket); }
bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
void nonblocking(Socket socket) { u_long enabled = 1; ioctlsocket(socket, FIONBIO, &enabled); }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void socket_close(Socket socket) { ::close(socket); }
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
void nonblocking(Socket socket) { fcntl(socket, F_SETFL, fcntl(socket, F_GETFL) | O_NONBLOCK); }
#endif
using Bytes = std::vector<unsigned char>;
using Message = std::shared_ptr<const Bytes>;
using Time = std::chrono::steady_clock;
Message frame(const Bytes& bytes, unsigned opcode = 2) {
  auto out = std::make_shared<Bytes>(); out->reserve(bytes.size() + 10); out->push_back(static_cast<unsigned char>(128 | opcode));
  if (bytes.size() < 126) out->push_back(static_cast<unsigned char>(bytes.size()));
  else if (bytes.size() <= 65535) { out->push_back(126); out->push_back(static_cast<unsigned char>(bytes.size() >> 8)); out->push_back(static_cast<unsigned char>(bytes.size())); }
  else { out->push_back(127); for (int i = 7; i >= 0; --i) out->push_back(static_cast<unsigned char>(uint64_t(bytes.size()) >> (8 * i))); }
  out->insert(out->end(), bytes.begin(), bytes.end()); return out;
}
void le64(Bytes& data, size_t at, uint64_t value) { for (unsigned i = 0; i < 8; ++i) data[at + i] = static_cast<unsigned char>(value >> (8 * i)); }
Message packet_frame(const StreamBuffer::Packet& packet, int kind) {
  Bytes data(40 + packet.data.size());
  data[0] = 'R'; data[1] = 'W'; data[2] = 'S'; data[3] = 1; data[4] = static_cast<unsigned char>(kind);
  le64(data, 8, packet.sequence); uint64_t timestamp; std::memcpy(&timestamp, &packet.timestamp, 8);
  le64(data, 16, timestamp); le64(data, 24, packet.dropped); le64(data, 32, packet.data.size());
  std::memcpy(data.data() + 40, packet.data.data(), packet.data.size()); return frame(data);
}
std::string lower(std::string value) { for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return value; }
std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r"), last = value.find_last_not_of(" \t\r");
  return first == std::string::npos ? "" : value.substr(first, last - first + 1);
}
}
struct StreamTransport::Impl {
  struct Client {
    Socket socket;
    explicit Client(Socket value) : socket(value) {}
    std::string input, token;
    std::shared_ptr<StreamBuffer> stream;
    Message output;
    std::deque<Message> waiting;
    size_t offset = 0, queued_bytes = 0;
    bool upgraded = false, inflight = false, closing = false, failed = false;
    Time::time_point progress = Time::now(), opened = Time::now();
  };
  StreamHub& hub;
  Socket listener = invalid_socket;
  uint16_t port = 0;
  std::atomic<bool> stopping{false};
  std::thread thread;
  std::vector<Client> clients;
  std::map<std::shared_ptr<StreamBuffer>, Message> latest;
  explicit Impl(StreamHub& owner) : hub(owner) {
#ifdef _WIN32
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw Error("TRANSPORT_UNAVAILABLE", "Cannot initialize local sockets");
#endif
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener == invalid_socket || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, 16)) {
      if (listener != invalid_socket) socket_close(listener);
      throw Error("TRANSPORT_UNAVAILABLE", "Cannot bind the local stream transport");
    }
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size); port = ntohs(address.sin_port);
    nonblocking(listener); thread = std::thread([this] { run(); });
  }
  ~Impl() {
    stopping.store(true); if (thread.joinable()) thread.join();
    for (auto& client : clients) socket_close(client.socket);
    socket_close(listener);
#ifdef _WIN32
    WSACleanup();
#endif
  }
  bool handshake(Client& client) {
    const auto end = client.input.find("\r\n\r\n"); if (end == std::string::npos) return client.input.size() <= 8192;
    std::istringstream input(client.input.substr(0, end)); std::string line, method, path, version;
    std::getline(input, line); std::istringstream start(line); start >> method >> path >> version;
    if (method != "GET" || version != "HTTP/1.1" || path.size() != 65 || path[0] != '/') return false;
    std::map<std::string, std::string> headers;
    while (std::getline(input, line)) {
      const auto colon = line.find(':'); if (colon == std::string::npos) return false;
      if (!headers.emplace(lower(line.substr(0, colon)), trim(line.substr(colon + 1))).second) return false;
    }
    if (headers["host"] != "127.0.0.1:" + std::to_string(port) || lower(headers["upgrade"]) != "websocket" ||
        lower(headers["connection"]).find("upgrade") == std::string::npos || headers["sec-websocket-version"] != "13" ||
        headers["sec-websocket-key"].size() != 24) return false;
    {
      std::lock_guard<std::mutex> lock(hub.mutex_); auto ticket = hub.tickets_.find(path.substr(1));
      if (ticket == hub.tickets_.end() || ticket->second.connected || ticket->second.origin != headers["origin"] ||
          Time::now() - ticket->second.created > std::chrono::seconds(10)) return false;
      ticket->second.connected = true; client.token = ticket->first; client.stream = ticket->second.buffer;
    }
    const auto key = headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[20]; char accept[32]; WDL_SHA1 sha; sha.add(key.data(), static_cast<int>(key.size())); sha.result(digest);
    wdl_base64encode(digest, accept, 20);
    const std::string response = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ") + accept + "\r\n\r\n";
    client.output = std::make_shared<Bytes>(response.begin(), response.end()); client.upgraded = true;
    const auto cached = latest.find(client.stream);
    if (cached != latest.end()) { client.waiting.push_back(cached->second); client.queued_bytes = cached->second->size(); }
    client.input.erase(0, end + 4); return true;
  }
  bool incoming(Client& client) {
    while (client.input.size() >= 2) {
      const auto* data = reinterpret_cast<const unsigned char*>(client.input.data());
      const auto opcode = data[0] & 15, length = data[1] & 127;
      if (!(data[0] & 128) || (data[0] & 112) || !(data[1] & 128) || length > 125) return false;
      if (client.input.size() < size_t(6 + length)) return true;
      if (opcode == 8) return false;
      if (opcode == 2 && length == 1 && (data[6] ^ data[2]) == 1) client.inflight = false;
      else if (opcode != 10) return false;
      client.input.erase(0, 6 + length); client.progress = Time::now();
    }
    return true;
  }
  void pump(Client& client) {
    char input[4096]; const auto read = recv(client.socket, input, sizeof(input), 0);
    if (!read || (read < 0 && !would_block())) { client.failed = true; return; }
    if (read > 0) {
      client.input.append(input, read);
      if (client.input.size() > 8192 || (!client.upgraded && !handshake(client)) || (client.upgraded && !incoming(client))) { client.failed = true; return; }
    }
    if (!client.upgraded) { if (Time::now() - client.opened > std::chrono::seconds(3)) client.failed = true; return; }
    {
      std::lock_guard<std::mutex> lock(hub.mutex_);
      if (!hub.tickets_.count(client.token)) { client.failed = true; return; }
    }
    if (!client.output) {
      const auto reason = client.stream->closed.load();
      if (reason) {
        const std::string code = StreamHub::code(reason); Bytes close{3, 232}; close.insert(close.end(), code.begin(), code.end());
        client.output = frame(close, 8); client.closing = true; client.waiting.clear(); client.queued_bytes = 0;
      } else if (!client.inflight && !client.waiting.empty()) {
        client.output = client.waiting.front(); client.waiting.pop_front(); client.queued_bytes -= client.output->size(); client.inflight = true;
      }
    }
    if (client.output) {
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const int length = static_cast<int>(std::min<size_t>(65536, client.output->size() - client.offset));
      const auto written = ::send(client.socket, reinterpret_cast<const char*>(client.output->data() + client.offset), length, flags);
      if (written < 0 && !would_block()) { client.failed = true; return; }
      if (written > 0) { client.offset += written; client.progress = Time::now(); }
      if (client.offset == client.output->size()) { client.output.reset(); client.offset = 0; if (client.closing) client.failed = true; }
    }
    if (Time::now() - client.progress > std::chrono::seconds(30) && (client.inflight || client.output)) client.failed = true;
  }
  void run() noexcept {
    try {
      while (!stopping.load()) {
        for (int count = 0; count < 4; ++count) {
          const auto connection = accept(listener, nullptr, nullptr); if (connection == invalid_socket) break;
          if (clients.size() >= 64) { socket_close(connection); continue; }
          nonblocking(connection); int on = 1;
          setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
#ifdef SO_NOSIGPIPE
          setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
          clients.push_back(Client{connection});
        }
        std::vector<std::shared_ptr<StreamBuffer>> buffers;
        {
          std::lock_guard<std::mutex> lock(hub.mutex_);
          for (const auto& slot : hub.slots_) if (slot.owned) buffers.push_back(slot.owned);
          for (auto it = hub.tickets_.begin(); it != hub.tickets_.end();)
            if (!it->second.connected && Time::now() - it->second.created > std::chrono::seconds(10)) it = hub.tickets_.erase(it); else ++it;
        }
        for (auto it = latest.begin(); it != latest.end();) {
          if (it->first->closed) it = latest.erase(it); else ++it;
        }
        for (const auto& buffer : buffers) {
          StreamBuffer::Packet packet;
          for (int i = 0; i < 64 && buffer->consume(packet); ++i) {
            Message message;
            if (buffer->kind != REAWEB_AUDIO && buffer->kind != REAWEB_MIDI) { message = packet_frame(packet, buffer->kind); latest[buffer] = message; }
            for (auto& client : clients) if (client.upgraded && client.stream == buffer && !client.failed) {
              if (buffer->kind != REAWEB_AUDIO && buffer->kind != REAWEB_MIDI) { client.waiting.clear(); client.queued_bytes = 0; }
              if (client.waiting.size() >= 8 || client.queued_bytes + packet.data.size() > 16 * 1024 * 1024) continue;
              if (!message) message = packet_frame(packet, buffer->kind);
              client.waiting.push_back(message); client.queued_bytes += message->size();
            }
          }
        }
        for (auto& client : clients) pump(client);
        for (auto it = clients.begin(); it != clients.end();) {
          if (!it->failed) { ++it; continue; }
          socket_close(it->socket);
          { std::lock_guard<std::mutex> lock(hub.mutex_); hub.tickets_.erase(it->token); }
          it = clients.erase(it);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } catch (...) {
      for (auto& client : clients) { socket_close(client.socket); std::lock_guard<std::mutex> lock(hub.mutex_); hub.tickets_.erase(client.token); }
      clients.clear();
    }
  }
};
StreamTransport::StreamTransport(StreamHub& hub) : impl_(std::make_unique<Impl>(hub)) {}
StreamTransport::~StreamTransport() = default;
std::string StreamTransport::url() const { return "ws://127.0.0.1:" + std::to_string(impl_->port); }
}
