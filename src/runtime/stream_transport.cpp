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
#include "runtime/jsfx_transfer.hpp"
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
Message packet_frame(const StreamBuffer::Packet& packet, int kind, bool project_video = false, std::shared_ptr<Bytes> reusable = {}) {
  if (project_video && !packet.timing) {
    const size_t length = 40 + packet.data.size();
    const size_t prefix = length < 126 ? 2 : length <= 65535 ? 4 : 10;
    auto out = reusable ? std::move(reusable) : std::make_shared<Bytes>();
    out->resize(prefix + length);
    std::fill(out->begin(), out->begin() + prefix + 40, 0);
    auto& data = *out; data[0] = 0x82;
    if (prefix == 2) data[1] = static_cast<unsigned char>(length);
    else {
      data[1] = prefix == 4 ? 126 : 127;
      for (size_t i = 2; i < prefix; ++i) data[i] = static_cast<unsigned char>(uint64_t(length) >> (8 * (prefix - 1 - i)));
    }
    data[prefix] = 'R'; data[prefix + 1] = 'W'; data[prefix + 2] = 'S'; data[prefix + 3] = 1; data[prefix + 4] = static_cast<unsigned char>(kind);
    data[prefix + 5] = packet.discontinuity ? 1 : 0;
    uint64_t timestamp; std::memcpy(&timestamp, &packet.timestamp, 8);
    le64(data, prefix + 8, packet.sequence); le64(data, prefix + 16, timestamp);
    le64(data, prefix + 24, packet.dropped); le64(data, prefix + 32, packet.data.size());
    std::memcpy(data.data() + prefix + 40, packet.data.data(), packet.data.size());
    return out;
  }
  const size_t header = packet.timing ? 80 : 40;
  Bytes data(header + packet.data.size());
  data[0] = 'R'; data[1] = 'W'; data[2] = 'S'; data[3] = packet.timing ? 2 : 1; data[4] = static_cast<unsigned char>(kind);
  le64(data, 8, packet.sequence); uint64_t timestamp; std::memcpy(&timestamp, &packet.timestamp, 8);
  le64(data, 16, timestamp); le64(data, 24, packet.dropped); le64(data, 32, packet.data.size());
  if (packet.timing) {
    const auto& timing = *packet.timing;
    le64(data, 40, timing.clock_epoch); le64(data, 48, timing.first_sample); le64(data, 56, timing.source_dropped);
    uint64_t position; std::memcpy(&position, &timing.position, 8); le64(data, 64, position);
    data[72] = static_cast<unsigned char>(timing.play_state); data[73] = timing.discontinuity ? 1 : 0;
  }
  std::memcpy(data.data() + header, packet.data.data(), packet.data.size()); return frame(data);
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
    std::shared_ptr<JsfxTransfer> reliable;
    Message output;
    std::deque<Message> waiting;
    size_t offset = 0, queued_bytes = 0;
    bool upgraded = false, closing = false, failed = false;
    unsigned inflight = 0;
    Time::time_point progress = Time::now(), opened = Time::now();
    std::deque<Time::time_point> frame_started;
  };
  StreamHub& hub;
  Socket listener = invalid_socket;
  uint16_t port = 0;
  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> connections{0}, handshake_failures{0}, expired_tickets{0};
  std::atomic<const char*> last_handshake_error{""};
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
  bool reject(const char* reason) {
    ++handshake_failures; last_handshake_error = reason; return false;
  }
  bool handshake(Client& client) {
    const auto end = client.input.find("\r\n\r\n"); if (end == std::string::npos) return client.input.size() <= 8192;
    std::istringstream input(client.input.substr(0, end)); std::string line, method, path, version;
    std::getline(input, line); std::istringstream start(line); start >> method >> path >> version;
    if (method != "GET" || version != "HTTP/1.1" || path.size() != 65 || path[0] != '/') return reject("INVALID_HANDSHAKE");
    std::map<std::string, std::string> headers;
    while (std::getline(input, line)) {
      const auto colon = line.find(':'); if (colon == std::string::npos) return reject("INVALID_HANDSHAKE");
      if (!headers.emplace(lower(line.substr(0, colon)), trim(line.substr(colon + 1))).second) return reject("INVALID_HANDSHAKE");
    }
    if (headers["host"] != "127.0.0.1:" + std::to_string(port) || lower(headers["upgrade"]) != "websocket" ||
        lower(headers["connection"]).find("upgrade") == std::string::npos || headers["sec-websocket-version"] != "13" ||
        headers["sec-websocket-key"].size() != 24) return reject("INVALID_HANDSHAKE");
    {
      std::lock_guard<std::mutex> lock(hub.mutex_); auto ticket = hub.tickets_.find(path.substr(1));
      if (ticket == hub.tickets_.end()) return reject("TICKET_NOT_FOUND");
      if (ticket->second.connected) return reject("TICKET_USED");
      if (!ticket->second.external && ticket->second.origin != headers["origin"]) return reject("ORIGIN_MISMATCH");
      if (Time::now() - ticket->second.created > std::chrono::seconds(10)) return reject("TICKET_EXPIRED");
      ticket->second.connected = true; client.token = ticket->first; client.stream = ticket->second.buffer;client.reliable=ticket->second.reliable;
    }
    if (client.stream && client.stream->kind == REAWEB_FRAME) {
      const int bytes = std::min<int>(8 * 1024 * 1024, int(client.stream->descriptor.max_bytes) + 50);
      setsockopt(client.socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    }
    const auto key = headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[20]; char accept[32]; WDL_SHA1 sha; sha.add(key.data(), static_cast<int>(key.size())); sha.result(digest);
    wdl_base64encode(digest, accept, 20);
    const std::string response = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ") + accept + "\r\n\r\n";
    client.output = std::make_shared<Bytes>(response.begin(), response.end()); client.upgraded = true;
    const auto cached = latest.find(client.stream);
    if (cached != latest.end()) { client.waiting.push_back(cached->second); client.queued_bytes = cached->second->size(); }
    ++connections; client.input.erase(0, end + 4); return true;
  }
  bool incoming(Client& client) {
    while (client.input.size() >= 2) {
      const auto* data = reinterpret_cast<const unsigned char*>(client.input.data());
      const auto opcode = data[0] & 15;
      uint64_t length=data[1]&127;size_t header=2;
      if (!(data[0] & 128) || (data[0] & 112) || !(data[1] & 128)) return false;
      if(length>125) {
        if(!client.reliable || opcode!=2)return false;
        const unsigned extended=length==126?2:8;
        if(client.input.size()<2+extended)return true;
        length=0;for(unsigned i=0;i<extended;++i)length=(length<<8)|data[2+i];header+=extended;
      }
      if(length>(client.reliable?JsfxTransfer::chunk_bytes+104:125))return false;
      if (client.input.size() < header+4+size_t(length)) return true;
      if (opcode == 8) return false;
      if(client.reliable && opcode==2) {
        if(client.waiting.size()>=2)return false;
        Bytes request(size_t(length),0);
        for(size_t i=0;i<request.size();++i)request[i]=data[header+4+i]^data[header+(i&3)];
        try {client.waiting.push_back(frame(client.reliable->exchange(request)));}
        catch(const Error&) {return false;}
      }
      else if (opcode == 2 && length == 1 && (data[header+4] ^ data[header]) == 1) {
        if (client.inflight) {
          if (client.stream && client.stream->kind == REAWEB_FRAME && !client.frame_started.empty()) {
            client.stream->frame_ack_ms = std::chrono::duration<double, std::milli>(Time::now() - client.frame_started.front()).count();
            client.frame_started.pop_front();
          }
          --client.inflight;
        }
      }
      else if (opcode != 10) return false;
      client.input.erase(0,header+4+size_t(length));client.progress=Time::now();
    }
    return true;
  }
  void pump(Client& client) {
    char input[4096]; const auto read = recv(client.socket, input, sizeof(input), 0);
    if (!read || (read < 0 && !would_block())) { client.failed = true; return; }
    if (read > 0) {
      client.input.append(input, read);
      if (client.input.size() > (client.reliable?JsfxTransfer::chunk_bytes+104+14:8192)) { if (!client.upgraded) reject("HANDSHAKE_TOO_LARGE"); client.failed = true; return; }
      if ((!client.upgraded && !handshake(client)) || (client.upgraded && !incoming(client))) { client.failed = true; return; }
    }
    if (!client.upgraded) {
      if (Time::now() - client.opened > std::chrono::seconds(3)) { reject("HANDSHAKE_TIMEOUT"); client.failed = true; }
      return;
    }
    {
      std::lock_guard<std::mutex> lock(hub.mutex_);
      if (!hub.tickets_.count(client.token)) { client.failed = true; return; }
    }
    if (!client.output) {
      const auto reason = client.reliable?(client.reliable->closed()||client.reliable->expired()?REAWEB_STREAM_CLOSED:0):client.stream->closed.load();
      if (reason) {
        client.waiting.clear(); client.queued_bytes = 0;
        // Drain the final frame ACK before closing a large buffered video socket.
        if (!client.stream || client.stream->kind != REAWEB_FRAME || !client.inflight) {
          const std::string code = StreamHub::code(reason); Bytes close{3, 232}; close.insert(close.end(), code.begin(), code.end());
          client.output = frame(close, 8); client.closing = true;
        }
      } else if ((client.inflight < (client.stream && client.stream->project_video ? 2u : 1u) || client.reliable) && !client.waiting.empty()) {
        client.output = client.waiting.front(); client.waiting.pop_front();
        if(!client.reliable){client.queued_bytes -= client.output->size();++client.inflight;if(client.stream->kind==REAWEB_FRAME)client.frame_started.push_back(Time::now());}
      }
    }
    if (client.output) {
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      // Video frames must not be throttled to one 64 KiB write per timer wake.
      // Nonblocking writes retain bounded, per-consumer ACK backpressure.
      const size_t limit = client.stream && client.stream->kind == REAWEB_FRAME ? 8 * 1024 * 1024 : 65536;
      const int length = static_cast<int>(std::min<size_t>(limit, client.output->size() - client.offset));
      const auto written = ::send(client.socket, reinterpret_cast<const char*>(client.output->data() + client.offset), length, flags);
      if (written < 0 && !would_block()) { client.failed = true; return; }
      if (written > 0) { client.offset += written; client.progress = Time::now(); }
      if (client.offset == client.output->size()) { client.output.reset(); client.offset = 0; if (client.closing) client.failed = true; }
    }
    if (Time::now() - client.progress > std::chrono::seconds(30) && (client.inflight || client.output)) client.failed = true;
  }
  void run() noexcept {
#ifdef _WIN32
    struct VideoWait {
      HANDLE timer = nullptr;
      bool attempted = false;
      ~VideoWait() { if (timer) CloseHandle(timer); }
      bool wait() {
        if (!attempted) {
          attempted = true;
          timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        }
        LARGE_INTEGER due{}; due.QuadPart = -10000;
        return timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) &&
          WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0;
      }
    } video_wait;
#endif
    try {
      StreamBuffer::Packet video_packet{};
      std::vector<std::shared_ptr<Bytes>> video_messages;
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
#ifdef _WIN32
        bool project_video = false;
#endif
        {
          std::lock_guard<std::mutex> lock(hub.mutex_);
          for (const auto& slot : hub.slots_) if (slot.owned) {
            buffers.push_back(slot.owned);
#ifdef _WIN32
            if (slot.owned->project_video) project_video = true;
#endif
          }
          for (auto it = hub.tickets_.begin(); it != hub.tickets_.end();)
            if (!it->second.connected && Time::now() - it->second.created > std::chrono::seconds(10)) { ++expired_tickets; it = hub.tickets_.erase(it); } else ++it;
        }
        for (auto it = latest.begin(); it != latest.end();) {
          if (it->first->closed) it = latest.erase(it); else ++it;
        }
        for (const auto& buffer : buffers) {
          StreamBuffer::Packet ordinary_packet;
          auto& packet = buffer->project_video ? video_packet : ordinary_packet;
          for (int i = 0; i < 64 && buffer->consume(packet); ++i) {
            Message message;
            if (buffer->kind != REAWEB_AUDIO && buffer->kind != REAWEB_MIDI) {
              std::shared_ptr<Bytes> reusable;
              if (buffer->project_video) {
                for (const auto& candidate : video_messages) if (candidate.use_count() == 1) { reusable = candidate; break; }
                if (!reusable && video_messages.size() < 3) {
                  reusable = std::make_shared<Bytes>(); video_messages.push_back(reusable);
                }
              }
              message = packet_frame(packet, buffer->kind, buffer->project_video, std::move(reusable)); latest[buffer] = message;
            }
            for (auto& client : clients) if (client.upgraded && client.stream == buffer && !client.failed) {
              if (buffer->project_video) {
                // Drain ACKs and queued output before admitting a burst of captured frames.
                pump(client);
                if (client.failed) continue;
                // Absorb a brief ACK delay without accumulating a slow consumer's backlog.
                while (client.waiting.size() >= 2) {
                  client.queued_bytes -= client.waiting.front()->size(); client.waiting.pop_front();
                }
              } else if (buffer->kind != REAWEB_AUDIO && buffer->kind != REAWEB_MIDI) { client.waiting.clear(); client.queued_bytes = 0; }
              if (client.waiting.size() >= 8 || client.queued_bytes + packet.data.size() > 16 * 1024 * 1024) continue;
              if (!message) message = packet_frame(packet, buffer->kind);
              client.waiting.push_back(message); client.queued_bytes += message->size();
            }
          }
        }
        if (video_packet.data.capacity() && std::none_of(buffers.begin(), buffers.end(),
            [](const auto& buffer) { return buffer->project_video; })) {
          Bytes().swap(video_packet.data); video_messages.clear();
        }
        for (auto& client : clients) pump(client);
        for (auto it = clients.begin(); it != clients.end();) {
          if (!it->failed) { ++it; continue; }
          socket_close(it->socket);
          { std::lock_guard<std::mutex> lock(hub.mutex_); hub.tickets_.erase(it->token); }
          it = clients.erase(it);
        }
        // A coarse Windows sleep can consume an entire video frame's ACK budget.
#ifdef _WIN32
        if (project_video && video_wait.wait()) continue;
#endif
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
Json StreamTransport::diagnostics() const {
  return {{"connections", impl_->connections.load()}, {"handshakeFailures", impl_->handshake_failures.load()},
    {"expiredTickets", impl_->expired_tickets.load()}, {"lastHandshakeError", impl_->last_handshake_error.load()}};
}
}
