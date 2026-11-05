#include "platform/linux/linux_overlay.hpp"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XInput2.h>
#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <set>

namespace reaweb {
struct LinuxOverlay::State {
  inline static std::map<Display*, State*> connections;
  inline static XErrorHandler previous_error = nullptr;
  Display* display = nullptr;
  int xerror = 0;
  std::set<unsigned> held;
  static int on_error(Display* display, XErrorEvent* event) {
    auto it = connections.find(display);
    if (it != connections.end()) { it->second->xerror = event->error_code; return 0; }
    return previous_error ? previous_error(display, event) : 0;
  }
  void check(const char* code) {
    XSync(display, False);
    queue_events();
    if (xerror) { const auto value = xerror; xerror = 0; throw Error(code, "X11 request failed: " + std::to_string(value)); }
  }
  ::Window host = 0, owner = 0, shaped_frame = 0;
  unsigned source = 0, pending_source = 0;
  int xi = 0;
  double zoom = 1;
  OverlayOptions requested, applied;
  bool layer_ready = false, mouse_ready = false;
  bool layer_established = false;
  std::chrono::steady_clock::time_point attached;
  std::function<void(std::string)> error;
  using Remove = int(*)(unsigned);
  Remove remove = reinterpret_cast<Remove>(dlsym(RTLD_DEFAULT, "g_source_remove"));
  static std::string unavailable() {
    if (getenv("WAYLAND_DISPLAY")) return "Fixed layering and input shaping require a native X11 session, not XWayland";
    if (!getenv("DISPLAY")) return "Native X11 display unavailable";
    if (!dlsym(RTLD_DEFAULT, "g_unix_fd_add_full") || !dlsym(RTLD_DEFAULT, "g_source_remove") || !dlsym(RTLD_DEFAULT, "g_idle_add_full")) return "REAPER GLib event source unavailable";
    return {};
  }
  static std::string layer_unavailable(Display* display) {
    Atom type; int format = 0; unsigned long count = 0, remaining; unsigned char* data = nullptr;
    const auto restack = XInternAtom(display, "_NET_RESTACK_WINDOW", False);
    bool found = false;
    if (XGetWindowProperty(display, DefaultRootWindow(display), XInternAtom(display, "_NET_SUPPORTED", False),
        0, 65536, False, XA_ATOM, &type, &format, &count, &remaining, &data) == Success && format == 32) {
      const auto atoms = reinterpret_cast<Atom*>(data);
      found = std::find(atoms, atoms + count, restack) != atoms + count;
    }
    if (data) XFree(data);
    return found ? "" : "The X11 window manager does not support _NET_RESTACK_WINDOW";
  }
  ~State() {
    if (source && remove) remove(source);
    if (pending_source && remove) remove(pending_source);
    if (display) {
      XCloseDisplay(display); connections.erase(display);
      if (connections.empty()) XSetErrorHandler(previous_error);
    }
  }
  bool buttons() const {
    ::Window root, child; int rx, ry, x, y; unsigned mask = 0;
    XQueryPointer(display, DefaultRootWindow(display), &root, &child, &rx, &ry, &x, &y, &mask);
    return !held.empty() || (mask & (Button1Mask | Button2Mask | Button3Mask));
  }
  void connect() {
    if (display) return;
    const auto reason = unavailable();
    if (!reason.empty()) throw Error(requested.layer == "overlay" ? "LAYER_UNAVAILABLE" : "MOUSE_MODE_UNAVAILABLE", reason);
    display = XOpenDisplay(nullptr);
    if (!display) throw Error("MOUSE_MODE_UNAVAILABLE", "Cannot open the X11 input connection");
    if (connections.empty()) previous_error = XSetErrorHandler(on_error);
    connections[display] = this;
    if (requested.layer == "overlay") {
      const auto reason = layer_unavailable(display);
      if (!reason.empty()) throw Error("LAYER_UNAVAILABLE", reason);
    }
    int event, error_base, major = 2, minor = 2, shape_major = 0, shape_minor = 0;
    if (!XShapeQueryVersion(display, &shape_major, &shape_minor) || (shape_major == 1 && shape_minor < 1) ||
        !XQueryExtension(display, "XInputExtension", &xi, &event, &error_base) || XIQueryVersion(display, &major, &minor) != Success || (major == 2 && minor < 1))
      throw Error("MOUSE_MODE_UNAVAILABLE", "X Shape 1.1 and XInput 2.1 are required");
    unsigned char bits[XIMaskLen(XI_RawButtonRelease)]{};
    XISetMask(bits, XI_RawButtonPress); XISetMask(bits, XI_RawButtonRelease);
    XIEventMask mask{XIAllMasterDevices, sizeof(bits), bits};
    XISelectEvents(display, DefaultRootWindow(display), &mask, 1);
    XSelectInput(display, DefaultRootWindow(display), SubstructureNotifyMask);
    if (host) XSelectInput(display, host, StructureNotifyMask);
    using Add = unsigned(*)(int, int, unsigned, int(*)(int, unsigned, void*), void*, void(*)(void*));
    source = reinterpret_cast<Add>(dlsym(RTLD_DEFAULT, "g_unix_fd_add_full"))(0, ConnectionNumber(display), 1 | 8 | 16 | 32,
      +[](int, unsigned condition, void* data) -> int {
        auto& self = *static_cast<State*>(data);
        if (condition & (8 | 16 | 32)) { self.error("MOUSE_MODE_FAILED: X11 event connection closed"); self.source = 0; return 0; }
        self.process_events();
        return 1;
      }, this, nullptr);
    if (!source) throw Error("MOUSE_MODE_FAILED", "Cannot observe X11 input and stacking events");
    check("MOUSE_MODE_FAILED");
  }
  void queue_events() {
    if (pending_source || !display || !XPending(display)) return;
    using Idle = unsigned(*)(int, int(*)(void*), void*, void(*)(void*));
    pending_source = reinterpret_cast<Idle>(dlsym(RTLD_DEFAULT, "g_idle_add_full"))(0, +[](void* data) -> int {
      auto& self = *static_cast<State*>(data); self.pending_source = 0; self.process_events(); return 0;
    }, this, nullptr);
  }
  void process_events() {
    try {
      bool order_changed = false, input_changed = false;
      for (unsigned count = 0; count < 256 && XPending(display); ++count) {
        XEvent event; XNextEvent(display, &event);
        if (event.type == GenericEvent && event.xcookie.extension == xi && XGetEventData(display, &event.xcookie)) {
          const bool release = event.xcookie.evtype == XI_RawButtonRelease;
          const auto button = static_cast<XIRawEvent*>(event.xcookie.data)->detail;
          if (button < 4 || button > 7) {
            if (release) held.erase(button); else held.insert(button);
          }
          XFreeEventData(display, &event.xcookie);
          if (release) input_changed = true;
        } else if (event.type == ConfigureNotify || event.type == MapNotify || event.type == UnmapNotify || event.type == ReparentNotify) {
          order_changed = true;
          if ((event.type == ConfigureNotify && (event.xconfigure.window == host || event.xconfigure.window == shaped_frame)) ||
              (event.type == ReparentNotify && event.xreparent.window == host)) input_changed = true;
        } else if (event.type == DestroyNotify && event.xdestroywindow.window == host) host = 0;
      }
      if (order_changed) repair();
      if (input_changed) apply();
    } catch (const Error& e) { error(e.code + ": " + e.what()); }
    catch (const std::exception& e) { error(std::string("MOUSE_MODE_FAILED: ") + e.what()); }
    queue_events();
  }
  ::Window frame(::Window window) const {
    if (!window) return 0;
    for (int i = 0; i < 32; ++i) {
      ::Window root, parent, *children = nullptr; unsigned count;
      if (!XQueryTree(display, window, &root, &parent, &children, &count)) return 0;
      if (children) XFree(children);
      if (parent == root) return window;
      window = parent;
    }
    return 0;
  }
  void repair() {
    if (!display || !host || !owner || requested.layer != "overlay") return;
    const auto target = frame(host), base = frame(owner);
    if (!target || !base || target == base) throw Error("LAYER_UNAVAILABLE", "Standalone X11 host and REAPER main window required");
    ::Window root, parent, *children = nullptr; unsigned count;
    if (!XQueryTree(display, DefaultRootWindow(display), &root, &parent, &children, &count)) throw Error("LAYER_FAILED", "Cannot inspect X11 stacking order");
    unsigned own = count, above = count;
    for (unsigned i = 0; i < count; ++i) { if (children[i] == base) own = i; if (children[i] == target) above = i; }
    std::set<::Window> overlays;
    for (const auto& entry : connections) if (entry.second->requested.layer == "overlay" && entry.second->owner == owner && entry.second->host)
      overlays.insert(frame(entry.second->host));
    bool contiguous = above > own && above < count;
    if (contiguous) for (unsigned i = own + 1; i < above; ++i) if (!overlays.count(children[i])) contiguous = false;
    ::Window sibling = owner; int mode = Above;
    if (own < count) for (unsigned i = own + 1; i < count; ++i) {
      if (children[i] == target || overlays.count(children[i])) continue;
      // EWMH uses client IDs, not the window manager's frame IDs.
      Atom type; int format; unsigned long length, remaining; unsigned char* data = nullptr;
      if (XGetWindowProperty(display, DefaultRootWindow(display), XInternAtom(display, "_NET_CLIENT_LIST", False),
          0, 65536, False, XA_WINDOW, &type, &format, &length, &remaining, &data) == Success && format == 32) {
        auto clients = reinterpret_cast<unsigned long*>(data);
        for (unsigned long j = 0; j < length; ++j) if (frame(clients[j]) == children[i]) { sibling = clients[j]; mode = Below; break; }
      }
      if (data) XFree(data);
      break;
    }
    if (children) XFree(children);
    if (own < count && above < count && !contiguous) {
      XEvent request{}; request.xclient.type = ClientMessage; request.xclient.window = host;
      request.xclient.message_type = XInternAtom(display, "_NET_RESTACK_WINDOW", False);
      request.xclient.format = 32; request.xclient.data.l[0] = 2;
      request.xclient.data.l[1] = sibling; request.xclient.data.l[2] = mode;
      if (!XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &request))
        throw Error("LAYER_FAILED", "Cannot request X11 overlay ordering");
      XFlush(display);
    }
    check("LAYER_FAILED");
    layer_ready = contiguous;
    if (contiguous) layer_established = true;
  }
  void apply() {
    if (!display || !host || buttons()) return;
    // Publish the client and WM frame input shapes as one server transaction.
    struct Transaction {
      Display* display;
      explicit Transaction(Display* value) : display(value) { XGrabServer(display); }
      ~Transaction() { XUngrabServer(display); XFlush(display); }
    } transaction(display);
    if (requested.mode == "normal") XShapeCombineMask(display, host, ShapeInput, 0, 0, None, ShapeSet);
    else {
      XWindowAttributes bounds{};
      if (!XGetWindowAttributes(display, host, &bounds)) throw Error("MOUSE_MODE_FAILED", "X11 host unavailable");
      if (bounds.width > 32767 || bounds.height > 32767) throw Error("MOUSE_MODE_UNAVAILABLE", "X11 input regions require a viewport smaller than 32768 native pixels per axis");
      std::vector<XRectangle> rectangles;
      if (requested.mode == "regions") for (const auto& r : requested.regions) {
        const double left = std::clamp(r.x * zoom, 0.0, double(bounds.width));
        const double top = std::clamp(r.y * zoom, 0.0, double(bounds.height));
        const double right = std::clamp((r.x + r.width) * zoom, 0.0, double(bounds.width));
        const double bottom = std::clamp((r.y + r.height) * zoom, 0.0, double(bounds.height));
        if (right > left && bottom > top) rectangles.push_back({short(std::floor(left)), short(std::floor(top)),
          static_cast<unsigned short>(std::ceil(right) - std::floor(left)), static_cast<unsigned short>(std::ceil(bottom) - std::floor(top))});
      }
      XShapeCombineRectangles(display, host, ShapeInput, 0, 0, rectangles.data(), int(rectangles.size()), ShapeSet, Unsorted);
    }
    const auto outer = frame(host);
    if (outer && outer != host) {
      if (requested.mode == "normal") XShapeCombineMask(display, outer, ShapeInput, 0, 0, None, ShapeSet);
      else {
        XWindowAttributes bounds{}; XGetWindowAttributes(display, host, &bounds);
        ::Window child; int x = 0, y = 0;
        XTranslateCoordinates(display, host, outer, 0, 0, &x, &y, &child);
        XRectangle client{short(x), short(y), static_cast<unsigned short>(bounds.width), static_cast<unsigned short>(bounds.height)};
        // The WM frame otherwise catches holes in its client and swallows the input.
        XShapeCombineMask(display, outer, ShapeInput, 0, 0, None, ShapeSet);
        XShapeCombineRectangles(display, outer, ShapeInput, 0, 0, &client, 1, ShapeSubtract, Unsorted);
        XShapeCombineShape(display, outer, ShapeInput, x, y, host, ShapeInput, ShapeUnion);
      }
      shaped_frame = outer;
    }
    check("MOUSE_MODE_FAILED"); applied = requested; mouse_ready = true;
  }
};
LinuxOverlay::LinuxOverlay(const OverlayOptions& options, std::function<void(std::string)> error) : state_(std::make_unique<State>()) {
  state_->requested = options; state_->error = std::move(error);
  if (options.floating_only()) state_->connect();
}
LinuxOverlay::~LinuxOverlay() = default;
void LinuxOverlay::attach(unsigned long host, unsigned long owner) {
  auto& s = *state_;
  if (s.host == host && s.owner == owner) {
    if (s.display && host && s.requested.layer == "overlay" && !s.layer_established &&
        std::chrono::steady_clock::now() - s.attached > std::chrono::seconds(5)) {
      XWindowAttributes attributes{};
      if (XGetWindowAttributes(s.display, host, &attributes) && attributes.map_state == IsViewable)
        throw Error("LAYER_FAILED", "The X11 window manager did not establish overlay ordering within 5 seconds");
    }
    return;
  }
  if (s.display && s.shaped_frame && s.host && s.host != host) {
    XShapeCombineMask(s.display, s.shaped_frame, ShapeInput, 0, 0, None, ShapeSet);
    s.shaped_frame = 0; s.check("MOUSE_MODE_FAILED");
  }
  s.host = host; s.owner = owner;
  s.attached = std::chrono::steady_clock::now(); s.layer_established = false;
  if (s.display && host) { XSelectInput(s.display, host, StructureNotifyMask); s.apply(); s.repair(); }
}
void LinuxOverlay::scale(double value) { if (state_->zoom != value) { state_->zoom = value; state_->apply(); } }
void LinuxOverlay::mode(const std::string& value) {
  auto& s = *state_;
  if (value != "normal") {
    try { s.connect(); } catch (...) {
      if (s.source && s.remove) { s.remove(s.source); s.source = 0; }
      if (s.pending_source && s.remove) { s.remove(s.pending_source); s.pending_source = 0; }
      if (s.display) { XCloseDisplay(s.display); State::connections.erase(s.display); s.display = nullptr; if (State::connections.empty()) XSetErrorHandler(State::previous_error); }
      throw;
    }
  }
  const auto previous = s.requested; s.requested.mode = value;
  try { s.apply(); } catch (...) { s.requested = previous; s.apply(); throw; }
}
void LinuxOverlay::regions(const std::vector<MouseRegion>& value) {
  auto& s = *state_; const auto previous = s.requested; s.requested.regions = value;
  try { s.apply(); } catch (...) { s.requested = previous; s.apply(); throw; }
}
bool LinuxOverlay::layer_active() const { return state_->layer_ready; }
bool LinuxOverlay::mouse_active() const { return state_->requested.mode == "normal" || (state_->mouse_ready && state_->requested.mode == state_->applied.mode); }
Json LinuxOverlay::capabilities() {
  auto reason = State::unavailable();
  std::string layer_reason = reason;
  if (reason.empty()) {
    auto display = XOpenDisplay(nullptr);
    if (!display) reason = "X11 display unavailable";
    else {
      layer_reason = State::layer_unavailable(display);
      int shape_major = 0, shape_minor = 0, opcode = 0, event = 0, error = 0, major = 2, minor = 2;
      if (!XShapeQueryVersion(display, &shape_major, &shape_minor) || (shape_major == 1 && shape_minor < 1) ||
          !XQueryExtension(display, "XInputExtension", &opcode, &event, &error) || XIQueryVersion(display, &major, &minor) != Success || (major == 2 && minor < 1))
        reason = "Requires X Shape 1.1 and XInput 2.1";
      XCloseDisplay(display);
    }
  }
  return overlay_capabilities(!reason.empty() ? reason : layer_reason, reason, "x11-shape-xinput2");
}
}
