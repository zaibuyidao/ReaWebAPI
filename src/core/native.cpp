#include "core/native_call.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <unordered_set>

namespace reaweb {
namespace {
constexpr size_t buffer_limit = 16 * 1024 * 1024;
constexpr size_t array_limit = 1024 * 1024;
constexpr size_t default_buffer = 65536;
template<class F> F function(const NativeContext& c, const char* name) {
  return reinterpret_cast<F>(c.resolve(name));
}
std::string type_name(std::string value) {
  while (!value.empty() && value.back() == '*') value.pop_back();
  return value;
}
double number(const Json& v) {
  if (!v.is_number() || !std::isfinite(v.get<double>()))
    throw Error("INVALID_ARGUMENT", "Expected a finite number");
  return v.get<double>();
}
template<class T> T integral(const Json& v) {
  const auto d = number(v);
  if (d != std::floor(d) || d < static_cast<double>(std::numeric_limits<T>::lowest()) ||
      d > std::min(9007199254740991.0, static_cast<double>(std::numeric_limits<T>::max())))
    throw Error("INVALID_ARGUMENT", "Integer is outside its native ABI range");
  return static_cast<T>(d);
}
bool boolean(const Json& v) {
  if (!v.is_boolean()) throw Error("INVALID_ARGUMENT", "Expected a boolean");
  return v.get<bool>();
}
std::string text(const Json& v) {
  if (!v.is_string()) throw Error("INVALID_ARGUMENT", "Expected a string");
  auto s = v.get<std::string>();
  if (s.size() > buffer_limit) throw Error("BUFFER_LIMIT", "String exceeds 16 MiB");
  return s;
}
const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
Json bytes(const char* p, size_t length) {
  if (length > buffer_limit) throw Error("BUFFER_LIMIT", "Binary result exceeds 16 MiB");
  std::string out;
  out.reserve((length + 2) / 3 * 4);
  for (size_t i = 0; i < length; i += 3) {
    const auto a = static_cast<unsigned char>(p[i]);
    const auto b = i + 1 < length ? static_cast<unsigned char>(p[i+1]) : 0;
    const auto c = i + 2 < length ? static_cast<unsigned char>(p[i+2]) : 0;
    out += alphabet[a >> 2]; out += alphabet[((a & 3) << 4) | (b >> 4)];
    out += i + 1 < length ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
    out += i + 2 < length ? alphabet[c & 63] : '=';
  }
  return Json{{"__reawebBytes", std::move(out)}};
}
std::string unbytes(const Json& v) {
  if (v.is_string()) return text(v); // Text MIDI events may be supplied as UTF-8.
  if (!v.is_object() || v.size() != 1 || !v.contains("__reawebBytes") || !v["__reawebBytes"].is_string())
    throw Error("INVALID_ARGUMENT", "Expected a Uint8Array or string");
  const auto& s = v["__reawebBytes"].get_ref<const std::string&>();
  if (s.size() % 4 || s.size() > (buffer_limit + 2) / 3 * 4)
    throw Error("BUFFER_LIMIT", "Invalid or oversized binary argument");
  std::string out;
  out.reserve(s.size() / 4 * 3);
  for (size_t i = 0; i < s.size(); i += 4) {
    unsigned int n = 0;
    int padding = 0;
    for (int j = 0; j < 4; ++j) {
      if (s[i+j] == '=') {
        if (i + 4 != s.size() || j < 2) throw Error("INVALID_ARGUMENT", "Invalid base64 padding");
        ++padding; n <<= 6;
      } else {
        const auto q = std::strchr(alphabet, s[i+j]);
        if (padding || s[i+j] == 0 || !q) throw Error("INVALID_ARGUMENT", "Invalid base64 data");
        n = (n << 6) | static_cast<unsigned int>(q - alphabet);
      }
    }
    out += static_cast<char>((n >> 16) & 255);
    if (padding < 2) out += static_cast<char>((n >> 8) & 255);
    if (!padding) out += static_cast<char>(n & 255);
  }
  return out;
}
}

Json encode_binary(const char* data, size_t size) { return bytes(data, size); }
std::string decode_binary(const Json& value) { return unbytes(value); }

struct NativeContext::Impl {
  Impl(Host& host, std::string session) : host(host), session(std::move(session)) {}
  struct Handle {
    void* pointer;
    std::string type, identity, owner, destroy;
    void* project;
  };
  Host& host;
  std::string session;
  uint64_t sequence = 0;
  size_t buffer_size = default_buffer;
  std::unordered_map<std::string, Handle> handles;
  std::unordered_map<void*, std::string> reverse;
  std::deque<std::string> scan;
  // Function addresses belong to REAPER for the lifetime of the extension.
  mutable std::unordered_map<std::string, void*> functions;
};
NativeContext::NativeContext(Host& host, std::string session)
  : impl_(std::make_unique<Impl>(host, std::move(session))) {}
NativeContext::~NativeContext() { reset(); }
void* NativeContext::resolve(const char* name) const {
  const auto found = impl_->functions.find(name);
  if (found != impl_->functions.end()) return found->second;
  auto address = impl_->host.native_function ? impl_->host.native_function(name) : nullptr;
  impl_->functions.emplace(name, address);
  return address;
}
void* NativeContext::current_project() const { return impl_->host.current_project(); }
size_t NativeContext::buffer_size() const { return impl_->buffer_size; }
size_t NativeContext::set_buffer_size(size_t size) {
  if (size < 4096 || size > buffer_limit) throw Error("INVALID_ARGUMENT", "Buffer capacity must be between 4096 bytes and 16 MiB");
  return impl_->buffer_size = size;
}
Json NativeContext::capabilities() const {
  Json available = Json::array(), unavailable = Json::array();
  for (const auto& entry : native_entries())
    (resolve(entry.name) ? available : unavailable).push_back(entry.name);
  return {{"available", available.size()}, {"unavailable", unavailable}, {"availableMethods", available}};
}
std::string NativeContext::identity(void* p, const std::string& type) const {
  if (type == "MediaTrack") {
    if (auto fn = function<GUID* (*)(MediaTrack*)>(*this, "GetTrackGUID")) {
      auto g = fn(static_cast<MediaTrack*>(p));
      return g ? std::string(reinterpret_cast<const char*>(g), sizeof(GUID)) : "";
    }
  }
  const char* api = type == "MediaItem" ? "GetSetMediaItemInfo_String" : type == "MediaItem_Take" ?
    "GetSetMediaItemTakeInfo_String" : type == "TrackEnvelope" ? "GetSetEnvelopeInfo_String" : nullptr;
  if (api) {
    char g[128]{};
    if (type == "MediaItem") {
      if (auto fn = function<bool (*)(MediaItem*, const char*, char*, bool)>(*this, api))
        if (fn(static_cast<MediaItem*>(p), "GUID", g, false)) return g;
    } else if (type == "MediaItem_Take") {
      if (auto fn = function<bool (*)(MediaItem_Take*, const char*, char*, bool)>(*this, api))
        if (fn(static_cast<MediaItem_Take*>(p), "GUID", g, false)) return g;
    } else if (auto fn = function<bool (*)(TrackEnvelope*, const char*, char*, bool)>(*this, api))
      if (fn(static_cast<TrackEnvelope*>(p), "GUID", g, false)) return g;
  }
  return {};
}
std::string NativeContext::parent_token(const Json& args) const {
  for (const auto& a : args) if (a.is_object() && a.contains("id") && a["id"].is_string()) {
    const auto found = impl_->handles.find(a["id"].get<std::string>());
    if (found != impl_->handles.end()) return found->first;
  }
  return {};
}
Json NativeContext::handle(void* p, const std::string& type, void* project,
                          const std::string& owner, const std::string& destroy) {
  if (!p) return nullptr;
  if (!project) project = current_project();
  if (!owner.empty()) {
    const auto parent = impl_->handles.find(owner);
    if (parent != impl_->handles.end()) project = parent->second.project;
  }
  if (type == "ReaProject") project = p;
  auto ident = identity(p, type);
  if (type == "ProjectMarker") {
    char guid[128]{};
    if (auto fn = function<bool (*)(ReaProject*, ProjectMarker*, const char*, char*, bool)>(*this, "GetSetRegionOrMarkerInfo_String"))
      if (fn(static_cast<ReaProject*>(project), static_cast<ProjectMarker*>(p), "GUID", guid, false)) ident = guid;
  }
  if (auto it = impl_->reverse.find(p); it != impl_->reverse.end()) {
    auto& old = impl_->handles.at(it->second);
    if (old.type == type && old.identity == ident) return {{"type", type}, {"id", it->second}};
    impl_->handles.erase(it->second); impl_->reverse.erase(it);
  }
  // Bounded pruning prevents ordinary edits from accumulating dead tokens forever.
  for (int n = 0; n < 8 && !impl_->scan.empty(); ++n) {
    auto token = std::move(impl_->scan.front()); impl_->scan.pop_front();
    auto old = impl_->handles.find(token);
    if (old == impl_->handles.end()) continue;
    try {
      pointer(Json{{"type", old->second.type}, {"id", token}}, old->second.type);
      impl_->scan.push_back(std::move(token));
    } catch (const Error& e) { if (e.code != "STALE_HANDLE") throw; }
  }
  if (impl_->handles.size() >= 65536) throw Error("HANDLE_LIMIT", "Too many live object handles");
  auto token = impl_->session + ":" + std::to_string(++impl_->sequence);
  Impl::Handle value{p, type, ident, owner, destroy, project};
  impl_->handles.emplace(token, std::move(value)); impl_->reverse[p] = token;
  impl_->scan.push_back(token);
  return {{"type", type}, {"id", token}};
}
void* NativeContext::pointer(const Json& v, const std::string& type, bool validate) {
  if (v.is_null() || ((type == "ReaProject" || type == "HWND" || type == "IReaperControlSurface" || type == "KbdSectionInfo" || type == "void") && v == 0)) return nullptr;
  if (!v.is_object() || !v.contains("id") || !v["id"].is_string() || !v.contains("type") || !v["type"].is_string())
    throw Error("INVALID_HANDLE", "Expected an opaque " + type + " handle");
  const auto it = impl_->handles.find(v["id"].get<std::string>());
  if (it == impl_->handles.end()) {
    if (!validate) return nullptr; // ValidatePtr* can test an already invalidated token.
    throw Error("STALE_HANDLE", "Object handle is no longer live");
  }
  const auto& h = it->second;
  if (v["type"] != h.type || (type != "void" && h.type != type))
    throw Error("INVALID_HANDLE", "Object handle has the wrong native type");
  if (!validate) return h.pointer;
  bool valid = true;
  if (h.type == "HWND") valid = !impl_->host.valid_window || impl_->host.valid_window(h.pointer);
  else if (h.type == "PCM_source" && h.destroy.empty() && !h.owner.empty() &&
           impl_->handles.count(h.owner) && impl_->handles.at(h.owner).type == "PCM_source") {
    const auto parent = impl_->handles.at(h.owner);
    try {
      pointer(Json{{"type", parent.type}, {"id", h.owner}}, parent.type);
      auto fn = function<PCM_source* (*)(PCM_source*)>(*this, "GetMediaSourceParent");
      valid = fn && fn(static_cast<PCM_source*>(parent.pointer)) == h.pointer;
    } catch (const Error&) { valid = false; }
  }
  else if (h.type == "ProjectMarker") {
    const auto project_ok = function<bool (*)(ReaProject*, void*, const char*)>(*this, "ValidatePtr2");
    valid = project_ok && project_ok(nullptr, h.project, "ReaProject*");
    auto fn = function<ProjectMarker* (*)(ReaProject*, int, const char*)>(*this, "GetRegionOrMarker");
    valid = valid && fn && !h.identity.empty() && fn(static_cast<ReaProject*>(h.project), -1, h.identity.c_str()) == h.pointer;
  } else if (h.type == "MediaTrack" || h.type == "MediaItem" || h.type == "MediaItem_Take" ||
             h.type == "TrackEnvelope" || h.type == "ReaProject" || (h.type == "PCM_source" && h.destroy.empty())) {
    if (auto fn = function<bool (*)(ReaProject*, void*, const char*)>(*this, "ValidatePtr2")) {
      valid = h.type == "ReaProject" || fn(nullptr, h.project, "ReaProject*");
      if (valid) valid = fn(static_cast<ReaProject*>(h.project), h.pointer, (h.type + "*").c_str());
    }
    if (valid && !h.identity.empty()) valid = identity(h.pointer, h.type) == h.identity;
  }
  if (!valid) {
    impl_->reverse.erase(h.pointer); impl_->handles.erase(it);
    throw Error("STALE_HANDLE", "The native object was deleted or replaced");
  }
  return h.pointer;
}
void NativeContext::invalidate(void* p) {
  if (auto it = impl_->reverse.find(p); it != impl_->reverse.end()) {
    impl_->handles.erase(it->second); impl_->reverse.erase(it);
  }
}
void NativeContext::transfer(void* p, const std::string& owner) {
  const auto it = impl_->reverse.find(p);
  if (it == impl_->reverse.end()) return;
  auto& h = impl_->handles.at(it->second);
  h.owner = owner;
  h.destroy = owner.empty() ? "PCM_Source_Destroy" : "";
}
void NativeContext::reset() {
  // Accessors and unattached sources/joysticks created by this document are owned.
  for (auto& pair : impl_->handles) {
    auto& h = pair.second;
    if (h.destroy == "DestroyAudioAccessor") {
      if (auto fn = function<void (*)(AudioAccessor*)>(*this, h.destroy.c_str())) fn(static_cast<AudioAccessor*>(h.pointer));
    } else if (h.destroy == "PCM_Source_Destroy") {
      if (auto fn = function<void (*)(PCM_source*)>(*this, h.destroy.c_str())) fn(static_cast<PCM_source*>(h.pointer));
    } else if (h.destroy == "joystick_destroy") {
      if (auto fn = function<void (*)(joystick_device*)>(*this, h.destroy.c_str())) fn(static_cast<joystick_device*>(h.pointer));
    }
  }
  impl_->handles.clear(); impl_->reverse.clear(); impl_->scan.clear();
}
void NativeContext::validate(const NativeEntry& entry, const Json& args) {
  if (!resolve(entry.name)) throw Error("API_UNAVAILABLE", std::string(entry.name) + " is not available in this REAPER version");
  NativeFrame frame(*this, entry, args);
}
void NativeContext::validate_project(const NativeEntry& entry, const Json& args, void* project) {
  for (const auto& param : entry.parameters) {
    if (param.kind != Kind::Handle || param.input < 0 || static_cast<size_t>(param.input) >= args.size()) continue;
    const auto& value = args[param.input];
    if (value.is_object() && value.contains("$ref")) continue;
    auto p = pointer(value, param.handle);
    if (!p) continue;
    const auto it = impl_->reverse.find(p);
    if (it != impl_->reverse.end() && impl_->handles.at(it->second).project != project)
      throw Error("UNSUPPORTED_PROJECT", "Batches only accept objects from the current project");
  }
}
Json NativeContext::invoke(const NativeEntry& entry, const Json& args) {
  auto address = resolve(entry.name);
  if (!address) throw Error("API_UNAVAILABLE", std::string(entry.name) + " is not available in this REAPER version", {{"method", entry.name}});
  NativeFrame frame(*this, entry, args);
  const std::string name = entry.name;
  void* destroyed = nullptr;
  if (name == "PCM_Source_Destroy" || name == "joystick_destroy" || name == "DestroyAudioAccessor") {
    destroyed = pointer(args[0], "void");
    if (name == "PCM_Source_Destroy" && destroyed) {
      const auto it = impl_->reverse.find(destroyed);
      if (it != impl_->reverse.end() && impl_->handles.at(it->second).destroy.empty())
        throw Error("RESOURCE_IN_USE", "Detach the PCM source from its take before destroying it");
    }
  }
  PCM_source* old_source = nullptr;
  MediaItem_Take* take = nullptr;
  auto get_source = function<PCM_source* (*)(MediaItem_Take*)>(*this, "GetMediaItemTake_Source");
  if (name == "SetMediaItemTake_Source" && get_source) {
    take = static_cast<MediaItem_Take*>(pointer(args[0], "MediaItem_Take")); old_source = get_source(take);
  }
  entry.invoke(frame, address);
  if (frame.grow_read_buffer()) entry.invoke(frame, address);
  auto result = frame.finish();
  if (destroyed) invalidate(destroyed);
  if (take && result == true) {
    auto now = get_source(take);
    if (old_source && old_source != now) {
      handle(old_source, "PCM_source", current_project(), "", "PCM_Source_Destroy");
      transfer(old_source, "");
    }
    transfer(now, args[0]["id"].get<std::string>());
  }
  return result;
}

std::string NativeFrame::guid_string(const GUID& g) {
  char s[40];
  std::snprintf(s, sizeof(s), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
    static_cast<unsigned int>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1],
    g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return s;
}
NativeFrame::NativeFrame(NativeContext& context, const NativeEntry& entry, const Json& args)
  : context_(context), entry_(entry), args_(args), cells_(entry.parameters.size()),
    results_(Json::array()), owner_(context.parent_token(args)), project_(context.current_project()) {
  if (!args.is_array() || args.size() < static_cast<size_t>(entry.min_args) || args.size() > static_cast<size_t>(entry.max_args))
    throw Error("INVALID_ARGUMENT", "Wrong argument count");
  for (int i = 0; i < entry.return_count; ++i) results_.push_back(nullptr);
  try {
    for (size_t i = 0; i < cells_.size(); ++i) {
      auto& c = cells_[i]; const auto& p = entry.parameters[i];
      const bool present = p.input >= 0 && static_cast<size_t>(p.input) < args.size() && !args[p.input].is_null();
      const Json v = present ? args[p.input] : Json();
      if (p.input >= 0 && !present && !(p.flags & 4) && p.kind != Kind::Handle)
        throw Error("INVALID_ARGUMENT", "Required argument is missing", {{"argument", p.input}, {"method", entry.name}});
      switch (p.kind) {
        case Kind::Int: case Kind::IntPtr:
          if (present) c.integer = integral<int>(v);
          c.pointer = &c.integer; break;
        case Kind::UInt: case Kind::UIntPtr:
          if (present) c.uint = integral<unsigned int>(v);
          c.pointer = &c.uint; break;
        case Kind::Size: if (present) c.size = integral<size_t>(v); break;
        case Kind::Bool: case Kind::BoolPtr:
          if (present) c.boolean = boolean(v);
          c.pointer = &c.boolean; break;
        case Kind::Double: case Kind::DoublePtr:
          if (present) c.number = number(v);
          c.pointer = &c.number; break;
        case Kind::Handle:
          if (!present && !(p.flags & (4 | 8))) throw Error("INVALID_HANDLE", std::string("Expected a live ") + p.handle + " handle");
          c.pointer = context.pointer(v, p.handle, std::string(p.handle) != "void" ||
            (std::string(entry.name) != "ValidatePtr" && std::string(entry.name) != "ValidatePtr2"));
          if (std::string(p.handle) == "ReaProject") project_ = c.pointer ? c.pointer : context.current_project();
          break;
        case Kind::HandleOut: c.pointer = &c.object; break;
        case Kind::StringOut: c.pointer = &c.text; break;
        case Kind::String: case Kind::Buffer: {
          std::string value = present ? ((p.flags & 2) ? unbytes(v) : text(v)) : "";
          if (!(p.flags & 2) && value.find('\0') != std::string::npos)
            throw Error("INVALID_ARGUMENT", "Embedded NUL requires a binary API parameter");
          const size_t capacity = p.kind == Kind::Buffer ? std::max(context.buffer_size(), value.size() + 1) : value.size() + 1;
          c.storage.resize(capacity);
          std::copy(value.begin(), value.end(), c.storage.begin());
          c.buffer = c.storage.data();
          c.length = static_cast<int>(p.kind == Kind::Buffer ? capacity : value.size());
          c.pointer = c.buffer;
          if (p.flags & 1) {
            auto reg = function<int (*)(char**, int*)>(context, "realloc_cmd_register_buf");
            auto clear = function<void (*)(int)>(context, "realloc_cmd_clear");
            if (!reg || !clear) throw Error("API_UNAVAILABLE", "Dynamic buffers require REAPER 6.68 or newer");
            // REAPER tracks the original address and updates buffer/length on growth.
            realloc_tokens_.push_back(reg(&c.buffer, &c.length));
          }
          break;
        }
        case Kind::Length: case Kind::LengthPtr: break;
        case Kind::Guid: {
          if (present) {
            auto s = text(v);
            if (!s.empty()) {
              if (s.size() != 38 || s.front() != '{' || s.back() != '}')
                throw Error("INVALID_ARGUMENT", "GUID must use {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}");
              const auto hex = [&](size_t offset, size_t size) {
                unsigned int value = 0;
                for (size_t j = offset; j < offset + size; ++j) {
                  const char ch = s[j];
                  const int digit = ch >= '0' && ch <= '9' ? ch-'0' : ch >= 'A' && ch <= 'F' ? ch-'A'+10 : ch >= 'a' && ch <= 'f' ? ch-'a'+10 : -1;
                  if (digit < 0) throw Error("INVALID_ARGUMENT", "Invalid GUID digit");
                  value = (value << 4) | static_cast<unsigned int>(digit);
                }
                return value;
              };
              for (int j : {9,14,19,24}) if (s[j] != '-') throw Error("INVALID_ARGUMENT", "Invalid GUID separator");
              c.guid.Data1 = hex(1,8); c.guid.Data2 = static_cast<unsigned short>(hex(10,4)); c.guid.Data3 = static_cast<unsigned short>(hex(15,4));
              for (int k = 0; k < 8; ++k) c.guid.Data4[k] = static_cast<unsigned char>(hex(k < 2 ? 20+k*2 : 25+(k-2)*2,2));
            }
          }
          c.pointer = &c.guid; break;
        }
        case Kind::Rect:
          c.rect.left = integral<int>(args.at(p.input)); c.rect.top = integral<int>(args.at(p.input+1));
          c.rect.right = integral<int>(args.at(p.input+2)); c.rect.bottom = integral<int>(args.at(p.input+3));
          c.pointer = &c.rect; break;
        case Kind::Array:
          if (v.is_object() && v.size() == 1 && v.contains("__reawebFloat64")) {
            auto data = unbytes(Json{{"__reawebBytes", v["__reawebFloat64"]}});
            if (data.size() % 8 || data.size() / 8 > array_limit) throw Error("INVALID_ARGUMENT", "Invalid Float64Array length");
            c.samples.resize(data.size()/8);
            for (size_t n = 0; n < c.samples.size(); ++n) {
              uint64_t bits = 0;
              for (int b = 0; b < 8; ++b) bits |= uint64_t(static_cast<unsigned char>(data[n*8+b])) << (b*8);
              std::memcpy(&c.samples[n], &bits, 8);
            }
          } else {
            if (!v.is_array() || v.size() > array_limit) throw Error("INVALID_ARGUMENT", "Expected a numeric array of at most 1048576 elements");
            for (const auto& sample : v) c.samples.push_back(number(sample));
          }
          // All three array APIs write interleaved samples; peaks may use three blocks.
          {
            const auto channels = integral<int>(args.at(std::string(entry.name) == "GetAudioAccessorSamples" ? 2 : 3));
            const auto count = integral<int>(args.at(4));
            const auto blocks = std::string(entry.name) == "GetAudioAccessorSamples" ? 1 : (integral<int>(args.at(5)) ? 3 : 2);
            if (channels < 1 || count < 0 || static_cast<uint64_t>(channels) * count * blocks > c.samples.size())
              throw Error("INVALID_ARGUMENT", "Sample array is too small for the requested channel/sample count");
          }
          c.pointer = c.samples.data(); break;
      }
      if (p.input >= 0 && !present && (p.flags & 4) && p.output < 0) c.pointer = nullptr;
    }
    for (size_t i = 0; i < cells_.size(); ++i) {
      const auto& p = entry.parameters[i];
      if (p.kind == Kind::Length || p.kind == Kind::LengthPtr) {
        cells_[i].integer = cells_[p.link].length;
        cells_[i].pointer = &cells_[p.link].length;
      }
    }
  } catch (...) { cleanup(); throw; }
}
NativeFrame::~NativeFrame() { cleanup(); }
bool NativeFrame::grow_read_buffer() {
  const std::string name = entry_.name;
  if (name != "MIDI_GetEvt" && name != "MIDI_GetTextSysexEvt" && name != "MIDI_GetRecentInputEvent") return false;
  bool grew = false;
  for (size_t i = 0; i < cells_.size(); ++i) {
    auto& c = cells_[i]; const auto& p = entry_.parameters[i];
    if (p.kind != Kind::Buffer || c.length < 0 || c.buffer != c.storage.data() ||
        static_cast<size_t>(c.length) < c.storage.size()) continue;
    if (static_cast<size_t>(c.length) >= buffer_limit) throw Error("BUFFER_LIMIT", "MIDI event exceeds 16 MiB");
    c.storage.resize(static_cast<size_t>(c.length) + 1);
    c.length = static_cast<int>(c.storage.size());
    c.buffer = c.storage.data(); c.pointer = c.buffer; grew = true;
  }
  return grew;
}
void NativeFrame::cleanup() noexcept {
  if (auto clear = function<void (*)(int)>(context_, "realloc_cmd_clear"))
    for (auto it = realloc_tokens_.rbegin(); it != realloc_tokens_.rend(); ++it) clear(*it);
  realloc_tokens_.clear();
}
Json NativeFrame::export_pointer(void* pointer, std::string type) {
  type = type_name(type);
  const std::string name = entry_.name;
  const auto destroy = name == "CreateTakeAudioAccessor" || name == "CreateTrackAudioAccessor" ? "DestroyAudioAccessor" :
    name.rfind("PCM_Source_Create", 0) == 0 ? "PCM_Source_Destroy" : name == "joystick_create" ? "joystick_destroy" : "";
  return context_.handle(pointer, type, project_, owner_, destroy);
}
Json NativeFrame::finish() {
  Json arrays = Json::array();
  for (size_t i = 0; i < cells_.size(); ++i) {
    auto& c = cells_[i]; const auto& p = entry_.parameters[i];
    if (p.kind == Kind::Array) {
      std::string data(c.samples.size()*8, '\0');
      for (size_t n = 0; n < c.samples.size(); ++n) {
        uint64_t bits = 0; std::memcpy(&bits, &c.samples[n], 8);
        for (int b = 0; b < 8; ++b) data[n*8+b] = static_cast<char>((bits >> (b*8)) & 255);
      }
      arrays.push_back({{"index", p.input}, {"values", bytes(data.data(), data.size())}});
    }
    if (p.output < 0) continue;
    switch (p.kind) {
      case Kind::IntPtr: results_[p.output] = c.integer; break;
      case Kind::UIntPtr: results_[p.output] = c.uint; break;
      case Kind::BoolPtr: results_[p.output] = c.boolean; break;
      case Kind::DoublePtr: results_[p.output] = c.number; break;
      case Kind::StringOut: results_[p.output] = c.text ? Json(std::string(c.text)) : Json(nullptr); break;
      case Kind::Guid: results_[p.output] = guid_string(c.guid); break;
      case Kind::Rect:
        results_[p.output] = c.rect.left; results_[p.output+1] = c.rect.top;
        results_[p.output+2] = c.rect.right; results_[p.output+3] = c.rect.bottom; break;
      case Kind::HandleOut: results_[p.output] = export_pointer(c.object, p.handle); break;
      case Kind::Buffer: {
        if (c.length < 0 || static_cast<size_t>(c.length) > buffer_limit || !c.buffer)
          throw Error("BUFFER_LIMIT", "Native buffer exceeds 16 MiB");
        if (c.buffer == c.storage.data() && static_cast<size_t>(c.length) > c.storage.size())
          throw Error("BUFFER_LIMIT", "Native output requires a larger buffer", {{"requiredBytes", c.length}});
        if (p.flags & 2) results_[p.output] = bytes(c.buffer, static_cast<size_t>(c.length));
        else {
          bool counted = false;
          for (const auto& size : entry_.parameters)
            if (size.kind == Kind::LengthPtr && size.link == static_cast<int>(i)) counted = true;
          const auto end = static_cast<const char*>(std::memchr(c.buffer, 0, static_cast<size_t>(c.length)));
          if (!end && !counted) throw Error("BUFFER_LIMIT", "Native output filled its buffer without a terminator");
          const auto length = end ? static_cast<size_t>(end - c.buffer) : static_cast<size_t>(c.length);
          if (!counted && c.buffer == c.storage.data() && length + 1 == c.storage.size())
            throw Error("BUFFER_LIMIT", "Native output reached the fixed buffer capacity");
          results_[p.output] = std::string(c.buffer, length);
        }
        break;
      }
      default: throw Error("SCHEMA_MISMATCH", "Invalid native output mapping");
    }
  }
  Json value = results_.empty() ? Json(nullptr) : results_.size() == 1 ? results_[0] : results_;
  if (!arrays.empty()) return {{"__reawebCall", true}, {"value", value}, {"arrays", arrays}};
  return value;
}
}
