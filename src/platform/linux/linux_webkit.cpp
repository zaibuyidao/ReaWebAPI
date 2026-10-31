#include "platform/linux/linux_channel.hpp"
#include "platform/shared/navigation.hpp"
#include "platform/shared/page_background.hpp"
#include "platform/shared/drag_regions.hpp"
#include "platform/shared/idle_pool.hpp"
#include <gtk/gtk.h>
#include <sys/utsname.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <webkit2/webkit2.h>
#include <glib-unix.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <cstring>
#include <memory>
#include "platform/linux/gtk_drag.hpp"
#include "platform/linux/gtk_devtools.hpp"
#include "platform/linux/gtk_context_menu.hpp"
#include "platform/linux/gtk_resources.hpp"
#include "platform/linux/gtk_media.hpp"

namespace reaweb {
namespace {
bool window_moves_supported() {
  return gdk_x11_screen_supports_net_wm_hint(gdk_screen_get_default(), gdk_atom_intern_static_string("_NET_WM_MOVERESIZE"));
}
void open_external(const std::string& url) {
  validate_external_url(url);
  GError* error = nullptr;
  if (!gtk_show_uri_on_window(nullptr, url.c_str(), GDK_CURRENT_TIME, &error)) {
    std::string message = error ? error->message : "Cannot open external link";
    if (error) g_error_free(error);
    throw Error("EXTERNAL_OPEN_FAILED", message);
  }
}
class GtkIdleView {
  WebKitWebView* view_ = nullptr;
public:
  bool ready = false, failed = false;
  explicit GtkIdleView(WebKitWebContext* context) {
    view_ = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context, nullptr));
    g_object_ref_sink(view_);
    g_signal_connect(view_, "load-changed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent event, gpointer data) {
      if (event == WEBKIT_LOAD_FINISHED) static_cast<GtkIdleView*>(data)->ready = true;
    }), this);
    g_signal_connect(view_, "load-failed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent, const char*, GError*, gpointer data) -> gboolean {
      static_cast<GtkIdleView*>(data)->failed = true; return TRUE;
    }), this);
    g_signal_connect(view_, "web-process-terminated", G_CALLBACK(+[](WebKitWebView*, WebKitWebProcessTerminationReason, gpointer data) {
      static_cast<GtkIdleView*>(data)->failed = true;
    }), this);
    webkit_web_view_load_uri(view_, "about:blank");
  }
  ~GtkIdleView() {
    if (view_) { g_signal_handlers_disconnect_by_data(view_, this); gtk_widget_destroy(GTK_WIDGET(view_)); g_object_unref(view_); }
  }
  WebKitWebView* take() {
    g_signal_handlers_disconnect_by_data(view_, this);
    auto view = view_; view_ = nullptr; return view;
  }
};
class Page {
  LinuxChannel& channel_;
  int id_;
  std::string uri_;
  GtkWidget* plug_ = nullptr;
  WebKitWebView* view_ = nullptr;
  WebKitUserContentManager* manager_ = nullptr;
  std::shared_ptr<WebResources> resources_;
  std::unique_ptr<GtkMedia> media_;
  ::Window parent_ = 0;
  bool failed_ = false;
  bool hosted_ = false, allocating_host_ = false;
  bool loaded_ = false, allow_reload_ = false, intercept_reload_ = false;
  std::string navigation_uri_;
  std::unique_ptr<GtkNativeDrag> drag_;
  std::unique_ptr<GtkDevTools> devtools_;
  std::unique_ptr<GtkDockMenu> dock_menu_;
  bool focused_ = false;
  bool visible_ = false, mapped_ = false, geometry_initialized_ = false;
  int native_x_ = 0, native_y_ = 0, native_width_ = 0, native_height_ = 0;
  int allocated_width_ = 0, allocated_height_ = 0;
  ::Window watched_parent_ = 0;
  int watched_parent_width_ = 0, watched_parent_height_ = 0;
  gint64 last_parent_configure_ = 0;
  bool filter_installed_ = false;
  GdkRGBA background_{1, 1, 1, 1};
  DragRegions drag_regions_;
  bool frameless_ = false, resizable_ = true, docked_ = false;
  bool transparent_ = false, page_ready_ = false, alpha_pending_ = false;
  unsigned long alpha_parent_ = 0;
  void prepare_alpha() {
    if (!transparent_ || !page_ready_ || !parent_ || !mapped_ || alpha_pending_ || alpha_parent_ == parent_ || failed_) return;
    alpha_pending_ = true;
    const auto parent = parent_;
    webkit_web_view_get_snapshot(view_, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr,
      +[](GObject* source, GAsyncResult* result, gpointer data) {
        const auto parent = *static_cast<unsigned long*>(data);
        delete static_cast<unsigned long*>(data);
        GError* error = nullptr;
        auto surface = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &error);
        auto self = static_cast<Page*>(g_object_get_data(source, "reaweb-alpha"));
        if (self && !self->failed_) {
          self->alpha_pending_ = false;
          if (!surface || error) self->fail("TRANSPARENCY_FAILED: WebKitGTK could not prepare the Alpha frame");
          else if (self->parent_ == parent && self->mapped_) {
            self->alpha_parent_ = parent;
            try { self->channel_.send({{"id", self->id_}, {"op", "alpha-frame"}, {"parent", parent}}); }
            catch (const std::exception& failure) { self->fail(failure.what()); }
          } else self->prepare_alpha();
        }
        if (surface) cairo_surface_destroy(surface);
        if (error) g_error_free(error);
      }, new unsigned long(parent));
  }
  // The host learns a new size from REAPER's message loop, so it can only tell
  // this process about a resize as often as that loop runs. While the user drags
  // an edge, the window manager resizes the parent far more often than that.
  // Watching the parent directly lets the page follow at the rate the window
  // manager actually delivers, instead of waiting for the next host round trip.
  static GdkFilterReturn filterEvent(GdkXEvent* xevent, GdkEvent*, gpointer data) {
    return static_cast<Page*>(data)->onParentConfigure(static_cast<XEvent*>(xevent));
  }
  GdkFilterReturn onParentConfigure(XEvent* xevent) {
    if (xevent->type != ConfigureNotify || xevent->xconfigure.window != watched_parent_)
      return GDK_FILTER_CONTINUE;
    // Apply the parent's own change as a difference against the size this
    // process last saw. The frame around the client area is a constant width, so
    // the parent's change equals the client's change exactly, and a dropped
    // event is still covered by the next one instead of accumulating error.
    const int delta_width = xevent->xconfigure.width - watched_parent_width_;
    const int delta_height = xevent->xconfigure.height - watched_parent_height_;
    watched_parent_width_ = xevent->xconfigure.width;
    watched_parent_height_ = xevent->xconfigure.height;
    if ((!delta_width && !delta_height) || !geometry_initialized_ || !visible_ || !mapped_ || failed_)
      return GDK_FILTER_CONTINUE;
    last_parent_configure_ = g_get_monotonic_time();
    resize(std::max(1, native_width_ + delta_width), std::max(1, native_height_ + delta_height));
    return GDK_FILTER_CONTINUE;
  }
  // A host message carries the size REAPER had when its message loop last ran,
  // which can be one pass behind what was just applied here. While the parent is
  // actively reporting resizes, keep the size just applied instead of stepping
  // backwards; the window is short so a host size that the parent never echoes,
  // such as REAPER toggling its menu bar, is still applied once the drag stops.
  // That also bounds any disagreement between the two ways of computing the size.
  static constexpr gint64 drag_quiet_window_us_ = 200000;
  void preferAppliedSize(int& width, int& height) {
    if (!watched_parent_ || !last_parent_configure_) return;
    if (g_get_monotonic_time() - last_parent_configure_ > drag_quiet_window_us_) return;
    if (width == native_width_ && height == native_height_) return;
    if (native_width_ > 0 && native_height_ > 0) { width = native_width_; height = native_height_; }
  }
  void watchParent(::Window parent, bool docked) {
    // A docked window follows REAPER's own Docker layout, which the host reports
    // after every layout pass; watching it too would only duplicate that work.
    if (docked) parent = 0;
    if (parent == watched_parent_) return;
    auto gtk_display = gtk_widget_get_display(plug_);
    auto display = gdk_x11_display_get_xdisplay(gtk_display);
    if (watched_parent_) {
      gdk_x11_display_error_trap_push(gtk_display);
      XSelectInput(display, watched_parent_, NoEventMask);
      gdk_x11_display_error_trap_pop_ignored(gtk_display);
    }
    watched_parent_ = parent;
    watched_parent_width_ = watched_parent_height_ = 0;
    last_parent_configure_ = 0;
    // StructureNotifyMask is not exclusive, so REAPER keeps receiving its own
    // events for this window; this process only adds its own copy.
    gdk_x11_display_error_trap_push(gtk_display);
    XSelectInput(display, parent, StructureNotifyMask);
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, parent, &attributes)) {
      watched_parent_width_ = attributes.width;
      watched_parent_height_ = attributes.height;
    }
    gdk_x11_display_error_trap_pop_ignored(gtk_display);
  }
  void set_background(unsigned color) {
    if (transparent_) return;
    background_ = {((color >> 16) & 255) / 255.0, ((color >> 8) & 255) / 255.0, (color & 255) / 255.0, 1};
    webkit_web_view_set_background_color(view_, &background_);
    gdk_window_set_background_rgba(gtk_widget_get_window(plug_), &background_);
    gtk_widget_queue_draw(plug_);
    channel_.send({{"id", id_}, {"op", "background"}, {"color", color}});
  }
  void set_host_focus(bool focused) {
    if (focused_ == focused) return;
    focused_ = focused;
    // SWELL is not a GtkSocket. Supply XEmbed focus notifications so GtkPlug
    // propagates the host's focus and blur state to WebKit.
    auto display = gdk_x11_display_get_xdisplay(gtk_widget_get_display(plug_));
    XEvent message{};
    message.xclient.type = ClientMessage; message.xclient.display = display;
    message.xclient.window = gdk_x11_window_get_xid(gtk_widget_get_window(plug_));
    message.xclient.format = 32;
    message.xclient.message_type = XInternAtom(display, "_XEMBED", False);
    message.xclient.data.l[0] = CurrentTime;
    for (long opcode : {focused ? 1L : 2L, focused ? 4L : 5L}) {
      message.xclient.data.l[1] = opcode;
      XSendEvent(display, message.xclient.window, False, NoEventMask, &message);
    }
    XFlush(display);
  }
  void fail(const std::string& error) {
    if (!failed_) {
      failed_ = true;
      try { channel_.send({{"id", id_}, {"op", "error"}, {"error", error}}); }
      catch (...) { gtk_main_quit(); }
    }
  }
public:
  bool frame_event(GdkEvent* event) {
    if (!frameless_ || docked_ || failed_ || !parent_ || !visible_ || !mapped_ ||
        (event->type != GDK_BUTTON_PRESS && event->type != GDK_2BUTTON_PRESS)) return false;
    auto& button = event->button;
    if (button.button != 1 || button.send_event) return false;
    auto widget = gtk_get_event_widget(event);
    if (!widget || (widget != plug_ && !gtk_widget_is_ancestor(widget, plug_))) return false;
    auto device = gdk_event_get_device(event);
    GdkModifierType state{};
    if (!device) return false;
    gdk_device_get_state(device, gtk_widget_get_window(plug_), nullptr, &state);
    if (!(state & GDK_BUTTON1_MASK)) return false;
    auto display = gdk_x11_display_get_xdisplay(gtk_widget_get_display(plug_));
    const int scale = std::max(1, gtk_widget_get_scale_factor(plug_));
    const int root_x = std::lround(button.x_root * scale), root_y = std::lround(button.y_root * scale);
    int host_x = 0, host_y = 0; ::Window child = 0;
    XTranslateCoordinates(display, parent_, DefaultRootWindow(display), 0, 0, &host_x, &host_y, &child);
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(display, parent_, &attributes)) return false;
    const int x = root_x - host_x, y = root_y - host_y, edge = 6 * scale;
    if (x < 0 || y < 0 || x >= attributes.width || y >= attributes.height) return false;
    const bool left = x < edge, right = x >= attributes.width - edge;
    const bool top = y < edge, bottom = y >= attributes.height - edge;
    int direction = -1;
    if (resizable_) direction = top ? (left ? 0 : right ? 2 : 1) : bottom ? (left ? 6 : right ? 4 : 5) : left ? 7 : right ? 3 : -1;
    if (direction < 0 && (widget == GTK_WIDGET(view_) || gtk_widget_is_ancestor(widget, GTK_WIDGET(view_)))) {
      int vx = 0, vy = 0;
      gtk_widget_translate_coordinates(GTK_WIDGET(view_), plug_, 0, 0, &vx, &vy);
      int px = 0, py = 0; gdk_window_get_origin(gtk_widget_get_window(plug_), &px, &py);
      const double zoom = webkit_web_view_get_zoom_level(view_);
      if (drag_regions_.hit((button.x_root - px - vx) / zoom, (button.y_root - py - vy) / zoom,
          gtk_widget_get_allocated_width(GTK_WIDGET(view_)) / zoom, gtk_widget_get_allocated_height(GTK_WIDGET(view_)) / zoom)) direction = 8;
    }
    if (direction < 0) return false;
    XEvent message{};
    message.xclient.type = ClientMessage; message.xclient.display = display;
    message.xclient.window = parent_; message.xclient.format = 32;
    if (event->type == GDK_2BUTTON_PRESS) {
      if (direction != 8 || !resizable_) return true;
      message.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
      message.xclient.data.l[0] = 2;
      message.xclient.data.l[1] = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
      message.xclient.data.l[2] = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
      message.xclient.data.l[3] = 1;
    } else {
      message.xclient.message_type = XInternAtom(display, "_NET_WM_MOVERESIZE", False);
      message.xclient.data.l[0] = root_x; message.xclient.data.l[1] = root_y;
      message.xclient.data.l[2] = direction; message.xclient.data.l[3] = 1; message.xclient.data.l[4] = 1;
      gdk_device_ungrab(device, button.time);
      XUngrabPointer(display, button.time);
    }
    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &message);
    XFlush(display);
    return true;
  }
  Page(LinuxChannel& channel, WebKitWebContext* context, const Json& request, std::shared_ptr<GtkIdleView> prepared = {})
    : channel_(channel), id_(request.at("id").get<int>()), uri_(request.at("uri").get<std::string>()) {
    frameless_ = request.value("frameless", false); resizable_ = request.value("resizable", true);
    transparent_ = request.value("transparent", false);
    if (transparent_) {
      auto screen = gdk_screen_get_default();
      if (!gdk_screen_is_composited(screen) || !gdk_screen_get_rgba_visual(screen))
        throw Error("TRANSPARENCY_UNAVAILABLE", "Desktop Alpha requires an X11 compositor and RGBA visual");
      background_ = {0, 0, 0, 0};
    }
    if (frameless_ && !window_moves_supported()) throw Error("HOST_UNAVAILABLE", "Frameless windows require an X11 window manager with _NET_WM_MOVERESIZE support");
    intercept_reload_ = request.value("lifecycleReload", false);
    navigation_uri_ = uri_;
    if (prepared) {
      view_ = prepared->take();
      manager_ = webkit_web_view_get_user_content_manager(view_); g_object_ref(manager_);
    } else manager_ = webkit_user_content_manager_new();
    g_signal_connect(manager_, "script-message-received::reaweb", G_CALLBACK(+[](WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer data) {
      auto self = static_cast<Page*>(data);
      const auto uri = webkit_web_view_get_uri(self->view_);
      if (self->failed_ || !uri || !same_document(uri, self->uri_)) return;
      auto value = webkit_javascript_result_get_js_value(result);
      if (!jsc_value_is_string(value)) return;
      auto message = jsc_value_to_string(value);
      try {
        if (strlen(message) > message_limit) self->fail("JavaScript message limit exceeded");
        else if (self->drag_regions_.message(message)) {}
        else if (!page_background_message(message, [self](unsigned color) { self->set_background(color); }))
          self->channel_.send({{"id", self->id_}, {"op", "message"}, {"message", message}});
      } catch (const std::exception& error) { self->fail(error.what()); }
      g_free(message);
    }), this);
    if (!webkit_user_content_manager_register_script_message_handler(manager_, "reaweb")) {
      g_object_unref(manager_);
      throw std::runtime_error("Could not register WebKitGTK bridge");
    }
    const auto source = request.at("script").get<std::string>() +
      (transparent_ ? "" : page_background_script("webkit.messageHandlers.reaweb.postMessage(message);")) +
      drag_regions_script("webkit.messageHandlers.reaweb.postMessage(message);");
    auto script = webkit_user_script_new(source.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(manager_, script);
    webkit_user_script_unref(script);
    if (!prepared) view_ = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context, "user-content-manager", manager_, nullptr));
    if (transparent_) {
      g_object_set_data(G_OBJECT(view_), "reaweb-alpha", this);
      webkit_web_view_set_background_color(view_, &background_);
    }
    const auto resource_root = request.value("resourceRoot", std::string());
    if (!resource_root.empty()) {
      const auto end = uri_.find('/', 9);
      resources_ = std::make_shared<WebResources>(fs::u8path(resource_root), uri_.substr(9, end - 9));
      g_object_set_data(G_OBJECT(view_), "reaweb-resources", resources_.get());
    }
    if (!prepared) g_object_ref_sink(view_);
    channel_.send({{"id", id_}, {"op", "container"}, {"prewarmed", bool(prepared)}});
    auto settings = webkit_web_view_get_settings(view_);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_html5_local_storage(settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(settings, FALSE);
    webkit_settings_set_allow_universal_access_from_file_urls(settings, FALSE);
    g_signal_connect(view_, "decide-policy", G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer data) -> gboolean {
      auto self = static_cast<Page*>(data);
      if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) { webkit_policy_decision_ignore(decision); return TRUE; }
      if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        auto action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        auto uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
        if (!uri || !same_document(uri, self->uri_)) {
          const std::string target = uri ? uri : "";
          webkit_policy_decision_ignore(decision);
          if (!self->failed_) blocked_navigation(target, self->uri_, open_external, [self](const std::string& script) {
            webkit_web_view_evaluate_javascript(self->view_, script.c_str(), static_cast<gssize>(script.size()), nullptr, nullptr, nullptr, nullptr, nullptr);
          });
          return TRUE;
        }
        // WebKit sends navigation policy callbacks for in-document hash links.
        // Those must preserve the document and must not trigger cleanup.
        if (self->loaded_ && webkit_navigation_action_get_navigation_type(action) != WEBKIT_NAVIGATION_TYPE_RELOAD &&
            uri != self->navigation_uri_ && same_document(uri, self->navigation_uri_)) {
          self->navigation_uri_ = uri;
          return FALSE;
        }
        if (self->intercept_reload_ && self->loaded_ && !self->allow_reload_) {
          webkit_policy_decision_ignore(decision);
          try { self->channel_.send({{"id", self->id_}, {"op", "reload-request"}}); }
          catch (const std::exception& error) { self->fail(error.what()); }
          return TRUE;
        }
        self->allow_reload_ = false;
        self->navigation_uri_ = uri;
      }
      return FALSE;
    }), this);
    g_signal_connect(view_, "permission-request", G_CALLBACK(+[](WebKitWebView*, WebKitPermissionRequest* request, gpointer) -> gboolean {
      webkit_permission_request_deny(request); return TRUE;
    }), nullptr);
    g_signal_connect(view_, "web-process-terminated", G_CALLBACK(+[](WebKitWebView*, WebKitWebProcessTerminationReason, gpointer data) {
      static_cast<Page*>(data)->fail("WebKitGTK content process terminated. Reopen the tool.");
    }), this);
    g_signal_connect(view_, "load-failed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent, const char*, GError* error, gpointer data) -> gboolean {
      if (!g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
        static_cast<Page*>(data)->fail(error->message);
      return FALSE;
    }), this);
    g_signal_connect(view_, "load-changed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent event, gpointer data) {
      auto page = static_cast<Page*>(data);
      if (event == WEBKIT_LOAD_STARTED) page->page_ready_ = false;
      if (event == WEBKIT_LOAD_FINISHED) {
        page->page_ready_ = true; page->prepare_alpha();
        try { page->channel_.send({{"id", page->id_}, {"op", "loaded"}}); } catch (...) {}
      }
      if (event == WEBKIT_LOAD_STARTED) {
        auto self = static_cast<Page*>(data);
        if (self->resources_) self->resources_->clear_media();
        self->loaded_ = true;
        self->drag_regions_.clear();
        try { self->channel_.send({{"id", self->id_}, {"op", "navigating"}}); }
        catch (const std::exception& error) { self->fail(error.what()); }
      }
    }), this);
    drag_ = std::make_unique<GtkNativeDrag>(GTK_WIDGET(view_), [this](Json payload) {
      channel_.send({{"id", id_}, {"op", "drop"}, {"payload", payload}});
    });
    plug_ = gtk_plug_new(0);
    hosted_ = request.value("hosted", false);
    g_signal_connect_after(plug_, "size-allocate", G_CALLBACK((+[](GtkWidget* widget, GtkAllocation* allocation, gpointer data) {
      auto self = static_cast<Page*>(data);
      if (!self->hosted_ || self->allocating_host_ || !self->geometry_initialized_ || !self->parent_) return;
      const int width = self->allocated_width_, height = self->allocated_height_;
      if (width < 1 || height < 1) return;
      if (allocation->width == width && allocation->height == height) return;
      self->allocating_host_ = true;
      GtkAllocation bounds{0, 0, width, height};
      gtk_widget_size_allocate(widget, &bounds);
      gdk_window_resize(gtk_widget_get_window(widget), width, height);
      self->allocating_host_ = false;
    })), this);
    if (transparent_) gtk_widget_set_visual(plug_, gdk_screen_get_rgba_visual(gtk_widget_get_screen(plug_)));
    gtk_window_set_focus_on_map(GTK_WINDOW(plug_), FALSE);
    g_object_ref_sink(plug_);
    gtk_widget_set_app_paintable(plug_, TRUE);
    gtk_widget_set_redraw_on_allocate(plug_, FALSE);
    g_signal_connect(plug_, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t* dc, gpointer data) -> gboolean {
      const auto transparent = static_cast<Page*>(data)->transparent_;
      if (transparent) { cairo_save(dc); cairo_set_operator(dc, CAIRO_OPERATOR_SOURCE); }
      gdk_cairo_set_source_rgba(dc, &static_cast<Page*>(data)->background_);
      cairo_paint(dc);
      if (transparent) cairo_restore(dc);
      return FALSE;
    }), this);
    // GtkPlug emits delete-event when parked on the X11 root. The native host owns closing.
    g_signal_connect(plug_, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) -> gboolean { return TRUE; }), nullptr);
    devtools_ = std::make_unique<GtkDevTools>(view_, plug_, [this](Json state) {
      channel_.send({{"id", id_}, {"op", "devtools-state"}, {"state", std::move(state)}});
    });
    dock_menu_ = std::make_unique<GtkDockMenu>(view_, request.value("dockEnabled", false) ? std::function<void()>([this] {
      try { channel_.send({{"id", id_}, {"op", "dock-toggle"}}); }
      catch (const std::exception& error) { fail(error.what()); }
    }) : std::function<void()>{}, [this](DevToolsAction action) { devtools_->perform(action); },
      [this] { return devtools_->menu_state(); }, [this](int command) {
        try { channel_.send({{"id", id_}, {"op", "window-menu"}, {"command", command}}); }
        catch (const std::exception& error) { fail(error.what()); }
      });
    dock_menu_->set_app_name(request.value("appName", std::string()));
    gtk_widget_realize(plug_);
    // A global filter: events of the foreign parent window have no GdkWindow in
    // this process, so only GDK's display-wide filter list sees them.
    gdk_window_add_filter(nullptr, &Page::filterEvent, this);
    filter_installed_ = true;
    // Preserve presented pixels while WebKit prepares the next resized frame.
    XSetWindowAttributes attributes{};
    attributes.bit_gravity = NorthWestGravity;
    XChangeWindowAttributes(gdk_x11_display_get_xdisplay(gtk_widget_get_display(plug_)),
      gdk_x11_window_get_xid(gtk_widget_get_window(plug_)), CWBitGravity, &attributes);
    webkit_web_view_load_uri(view_, uri_.c_str());
  }
  void resize(int width, int height) {
    if (failed_ || !geometry_initialized_ || !parent_) return;
    width = std::max(1, width);
    height = std::max(1, height);
    const int scale = std::max(1, gtk_widget_get_scale_factor(plug_));
    const int gtk_width = std::max(1, static_cast<int>(std::lround(static_cast<double>(width) / static_cast<double>(scale))));
    const int gtk_height = std::max(1, static_cast<int>(std::lround(static_cast<double>(height) / static_cast<double>(scale))));
    const bool allocation_changed = allocated_width_ != gtk_width || allocated_height_ != gtk_height;
    const bool native_size_changed = native_width_ != width || native_height_ != height;
    if (!allocation_changed && !native_size_changed) return;
    auto display = gdk_x11_display_get_xdisplay(gtk_widget_get_display(plug_));
    if (allocation_changed) {
      GtkAllocation allocation{0, 0, gtk_width, gtk_height};
      allocated_width_ = gtk_width;
      allocated_height_ = gtk_height;
      gtk_widget_size_allocate(plug_, &allocation);
    }
    if (native_size_changed) {
      // Keep GDK's native-window cache in sync with the allocation. GDK
      // expects logical GTK units here and scales the X11 window itself.
      gdk_window_resize(gtk_widget_get_window(plug_), gtk_width, gtk_height);
      gdk_display_flush(gtk_widget_get_display(plug_));
    }
    native_width_ = width;
    native_height_ = height;
    XFlush(display);
  }
  ~Page() {
    failed_ = true;
    g_object_set_data(G_OBJECT(view_), "reaweb-alpha", nullptr);
    watchParent(0, false);
    if (filter_installed_) { gdk_window_remove_filter(nullptr, &Page::filterEvent, this); filter_installed_ = false; }
    dock_menu_.reset();
    drag_.reset();
    devtools_.reset();
    webkit_user_content_manager_unregister_script_message_handler(manager_, "reaweb");
    g_signal_handlers_disconnect_by_data(manager_, this);
    g_signal_handlers_disconnect_by_data(view_, this);
    webkit_web_view_stop_loading(view_);
    g_object_set_data(G_OBJECT(view_), "reaweb-resources", nullptr);
    gtk_widget_destroy(plug_);
    g_object_unref(view_); g_object_unref(manager_); g_object_unref(plug_);
  }
  void command(const Json& request) {
    const auto op = request.at("op").get<std::string>();
    if (op == "frameless") {
      frameless_ = request.at("value").get<bool>();
      const auto script = frameless_ && !docked_ ? "window.__reawebDragRegionsActive?.(true);" : "window.__reawebDragRegionsActive?.(false);";
      webkit_web_view_evaluate_javascript(view_, script, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
    } else if (op == "eval") {
      const auto script = request.at("script").get<std::string>();
      webkit_web_view_evaluate_javascript(view_, script.c_str(), static_cast<gssize>(script.size()), nullptr, nullptr, nullptr, nullptr, nullptr);
    } else if (op == "media-clear") {
      if (resources_) resources_->clear_media();
    } else if (op == "drop-enabled") {
      drag_->enabled(request.at("enabled").get<bool>());
    } else if (op == "reload") {
      allow_reload_ = true; webkit_web_view_reload(view_);
    } else if (op == "devtools") {
      devtools_->open();
    } else if (op == "devtools-restore") {
      devtools_->restore(request.at("state"));
    } else if (op == "devtools-action") {
      devtools_->perform(static_cast<DevToolsAction>(request.at("action").get<int>()));
    } else if (op == "resize") {
      int width = request.at("width").get<int>(), height = request.at("height").get<int>();
      preferAppliedSize(width, height);
      resize(width, height);
    } else if (op == "park") {
      // SWELL destroys its old X11 top-level when docking. Move out before that
      // happens, and stop watching it before its XID can be reused.
      set_host_focus(false);
      visible_ = false;
      mapped_ = false;
      watchParent(0, false);
      gtk_widget_hide(plug_);
      auto display = gdk_x11_display_get_xdisplay(gtk_widget_get_display(plug_));
      const auto xid = gdk_x11_window_get_xid(gtk_widget_get_window(plug_));
      XUnmapWindow(display, xid);
      XReparentWindow(display, xid, DefaultRootWindow(display), 0, 0);
      XSync(display, False);
      parent_ = 0;
      alpha_parent_ = 0;
      geometry_initialized_ = false;
      devtools_->owner(0);
      channel_.send({{"id", id_}, {"op", "parked"}});
    } else if (op == "geometry") {
      docked_ = request.value("docked", false);
      if (dock_menu_) {
        dock_menu_->set_docked(request.value("docked", false));
        dock_menu_->set_app_name(request.value("appName", std::string()));
      }
      const auto parent = request.at("parent").get<unsigned long>();
      const int x = request.at("x"), y = request.at("y");
      const int host_width = std::max(1, request.at("width").get<int>()), host_height = std::max(1, request.at("height").get<int>());
      // Start or stop watching this parent before the size below is resolved, so
      // the first event of a new parent is never attributed to the old one.
      watchParent(parent, request.value("docked", false));
      // Position, parent and visibility still come from the host; only the size
      // can already be stale, in which case the parent-derived size is newer.
      int width = host_width, height = host_height;
      preferAppliedSize(width, height);
      const int scale = std::max(1, gtk_widget_get_scale_factor(plug_));
      const auto to_gtk = [scale](int pixels) {
        return std::max(1, static_cast<int>(std::lround(static_cast<double>(pixels) / static_cast<double>(scale))));
      };
      const int gtk_width = to_gtk(width), gtk_height = to_gtk(height);
      // REAPER's SWELL geometry and the foreign X11 parent use the same
      // coordinates. GTK's allocation uses the helper's logical coordinates,
      // so convert only the GTK request using its runtime scale factor.
      auto display = gdk_x11_display_get_xdisplay(gtk_widget_get_display(plug_));
      auto xid = gdk_x11_window_get_xid(gtk_widget_get_window(plug_));
      auto gtk_display = gtk_widget_get_display(plug_);
      gdk_x11_display_error_trap_push(gtk_display);
      const bool parent_changed = parent != parent_;
      const bool position_changed = !geometry_initialized_ || native_x_ != x || native_y_ != y;
      const bool native_size_changed = !geometry_initialized_ || native_width_ != width || native_height_ != height;
      const bool allocation_changed = allocated_width_ != gtk_width || allocated_height_ != gtk_height;
      if (parent_changed) {
        if (mapped_) { XUnmapWindow(display, xid); mapped_ = false; }
        XReparentWindow(display, xid, parent, x, y);
        parent_ = parent;
        devtools_->owner(parent);
      }
      set_host_focus(request.value("focused", false));
      // GtkPlug is a GtkWindow. Set its logical default size before showing it
      // so GtkWindow does not replace the foreign-parent allocation with its
      // default 200x200 requisition. Once shown, repeating this top-level resize
      // request during a drag makes GTK renegotiate the embedded window each time.
      if (!visible_) gtk_window_resize(GTK_WINDOW(plug_), gtk_width, gtk_height);
      // A foreign REAPER/X11 parent is not a GtkSocket, so allocate the client
      // viewport explicitly. GTK's allocation and the X11 pixel size use
      // different units at a scaled display.
      GtkAllocation allocation{0, 0, gtk_width, gtk_height};
      if (allocation_changed) {
        allocated_width_ = gtk_width;
        allocated_height_ = gtk_height;
        gtk_widget_size_allocate(plug_, &allocation);
      }
      if (parent_changed || position_changed || native_size_changed) {
        if (auto gdk_window = gtk_widget_get_window(plug_)) {
          // GDK geometry uses the same logical units as GtkAllocation. Keep
          // its cached size in sync instead of resizing the X11 child behind
          // GTK's back.
          const int gtk_x = static_cast<int>(std::lround(static_cast<double>(x) / scale));
          const int gtk_y = static_cast<int>(std::lround(static_cast<double>(y) / scale));
          gdk_window_move_resize(gdk_window, gtk_x, gtk_y, gtk_width, gtk_height);
          gdk_display_flush(gtk_display);
        } else {
          XMoveResizeWindow(display, xid, x, y, static_cast<unsigned>(width), static_cast<unsigned>(height));
        }
      }
      const bool requested_visible = request.at("visible").get<bool>();
      if (requested_visible) {
        if (!visible_) {
          gtk_widget_show_all(plug_);
          visible_ = true;
          // The first allocation can happen while the plug is hidden. Repeat it
          // after showing so GtkPaned and WebKit receive the real client size.
          gtk_widget_size_allocate(plug_, &allocation);
        }
        if (!mapped_) { XMapWindow(display, xid); mapped_ = true; }
      } else {
        if (visible_) { gtk_widget_hide(plug_); visible_ = false; }
        if (mapped_) { XUnmapWindow(display, xid); mapped_ = false; }
      }
      native_x_ = x; native_y_ = y; native_width_ = width; native_height_ = height;
      geometry_initialized_ = true;
      XFlush(display);
      gdk_x11_display_error_trap_pop_ignored(gtk_widget_get_display(plug_));
      prepare_alpha();
    } else if (op == "focus" && parent_) {
      const bool focused = request.value("focused", true);
      set_host_focus(focused);
      if (focused) {
        gtk_widget_grab_focus(GTK_WIDGET(view_));
        auto display = gtk_widget_get_display(plug_);
        gdk_x11_display_error_trap_push(display);
        XSetInputFocus(gdk_x11_display_get_xdisplay(display), gdk_x11_window_get_xid(gtk_widget_get_window(plug_)), RevertToParent, CurrentTime);
        gdk_x11_display_error_trap_pop_ignored(display);
      }
    }
  }
  void start_drag(const Json& payload, std::function<void(Json)> reply) { drag_->start(payload, std::move(reply)); }
  Json media(const std::string& method, const Json& args) {
    if (!resources_) throw Error("HOST_UNAVAILABLE", "Media URLs require a local App document");
    if (!media_) media_ = std::make_unique<GtkMedia>(resources_);
    return method == "ReaWeb_OpenMedia" ? media_->open(fs::u8path(args.at(0).get<std::string>())) :
      Json(media_->close(args.at(0).get<std::string>()));
  }
};
struct Process {
  LinuxChannel channel{3};
  WebKitWebContext* context = nullptr;
  std::map<int, std::unique_ptr<Page>> pages;
  IdlePool<GtkIdleView> idle;
  Json last_idle;
  explicit Process(const char* data) {
    auto cache = (fs::path(data) / "Cache").string();
    auto manager = webkit_website_data_manager_new("base-data-directory", data, "base-cache-directory", cache.c_str(), nullptr);
    const auto cookies = (fs::path(data) / "cookies.sqlite").string();
    webkit_cookie_manager_set_persistent_storage(webkit_website_data_manager_get_cookie_manager(manager),
      cookies.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    context = webkit_web_context_new_with_website_data_manager(manager);
    register_app_resources(context);
    g_object_unref(manager);
    channel.send({{"op", "ready"}, {"protocol", 1}, {"version", REAWEB_VERSION}, {"frameless", window_moves_supported()}, {"browserVersion",
      std::to_string(webkit_get_major_version()) + "." + std::to_string(webkit_get_minor_version()) + "." + std::to_string(webkit_get_micro_version())}});
  }
  ~Process() { pages.clear(); idle.capacity(0); g_object_unref(context); }
  int fd() const { return channel.fd(); }
  void desktop(const Json& request) {
    const auto token = request.at("request").get<std::string>();
    auto respond = [&](Json response) { channel.send({{"op", "desktop-result"}, {"request", token}, {"response", response}}); };
    try {
      const auto method = request.at("method").get<std::string>();
      const auto& args = request.at("args");
      if (method == "ReaWeb_OpenMedia" || method == "ReaWeb_CloseMedia") {
        auto it = pages.find(request.at("id").get<int>());
        if (it == pages.end()) throw Error("WINDOW_CLOSED", "Media document is closed");
        respond({{"result", it->second->media(method, args)}}); return;
      }
      if (method == "ReaWeb_GetDisplays") {
        Json displays = Json::array(); auto display = gdk_display_get_default();
        for (int i = 0; i < gdk_display_get_n_monitors(display); ++i) {
          auto monitor = gdk_display_get_monitor(display, i); GdkRectangle bounds{}, work{};
          gdk_monitor_get_geometry(monitor, &bounds); gdk_monitor_get_workarea(monitor, &work);
          const auto rectangle = [](const GdkRectangle& r) { return Json{{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}}; };
          const auto scale = gdk_monitor_get_scale_factor(monitor);
          displays.push_back({{"id", std::to_string(i)}, {"bounds", rectangle(bounds)}, {"workArea", rectangle(work)},
            {"scaleFactor", scale}, {"dpi", 96 * scale}, {"primary", monitor == gdk_display_get_primary_monitor(display)}, {"units", "native"}});
        }
        respond({{"result", displays}}); return;
      }
      if (method == "ReaWeb_Drag") {
        auto it = pages.find(request.at("id").get<int>());
        if (it == pages.end()) throw Error("WINDOW_CLOSED", "Drag source window is closed");
        it->second->start_drag(args, [this, token](Json response) {
          channel.send({{"op", "desktop-result"}, {"request", token}, {"response", response}});
        }); return;
      }
      if (method == "ReaWeb_RevealPath") {
        auto path = args.at(0).get<std::string>();
        auto uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
        if (!uri) throw Error("INVALID_PATH", "Cannot convert path to a local file URI");
        struct Reveal { LinuxChannel* channel; std::string token, uri, parent; };
        auto parent = fs::path(path).parent_path().string();
        auto pending = new Reveal{&channel, token, uri, parent}; g_free(uri);
        g_bus_get(G_BUS_TYPE_SESSION, nullptr, +[](GObject*, GAsyncResult* result, gpointer data) {
          auto pending = static_cast<Reveal*>(data); GError* error = nullptr;
          auto bus = g_bus_get_finish(result, &error);
          auto fallback = +[](Reveal* p) {
            std::unique_ptr<Reveal> owner(p); GError* failure = nullptr;
            auto uri = g_filename_to_uri(p->parent.c_str(), nullptr, nullptr);
            const bool ok = uri && g_app_info_launch_default_for_uri(uri, nullptr, &failure); g_free(uri);
            Json response = ok ? Json{{"result", true}} : Json{{"error", {{"code", "REVEAL_FAILED"}, {"message", failure ? failure->message : "File manager unavailable"}}}};
            if (failure) g_error_free(failure);
            try { p->channel->send({{"op", "desktop-result"}, {"request", p->token}, {"response", response}}); } catch (...) {}
          };
          if (!bus) { if (error) g_error_free(error); fallback(pending); return; }
          const char* uris[] = {pending->uri.c_str(), nullptr};
          struct Call { Reveal* value; void (*fallback)(Reveal*); };
          auto call = new Call{pending, fallback};
          g_dbus_connection_call(bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", "ShowItems",
            g_variant_new("(^ass)", uris, ""), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
              std::unique_ptr<Call> call(static_cast<Call*>(data)); GError* error = nullptr;
              auto value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
              if (!value) { if (error) g_error_free(error); call->fallback(call->value); return; }
              g_variant_unref(value); std::unique_ptr<Reveal> p(call->value);
              try { p->channel->send({{"op", "desktop-result"}, {"request", p->token}, {"response", {{"result", true}}}}); } catch (...) {}
            }, call);
          g_object_unref(bus);
        }, pending); return;
      }
      if (method == "ReaWeb_OpenExternal") {
        open_external(args.at(0).get<std::string>());
        respond({{"result", true}}); return;
      }
      auto clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
      if (method == "ReaWeb_ClipboardWriteBinary") {
        auto bytes = std::make_unique<std::string>(decode_binary(args.at(1)));
        auto type = "ReaWebAPI.Binary:" + args.at(0).get<std::string>();
        GtkTargetEntry target{type.data(), 0, 0};
        if (!gtk_clipboard_set_with_data(clipboard, &target, 1,
          +[](GtkClipboard*, GtkSelectionData* selection, guint, gpointer data) {
            const auto& bytes = *static_cast<std::string*>(data);
            gtk_selection_data_set(selection, gtk_selection_data_get_target(selection), 8,
              reinterpret_cast<const guchar*>(bytes.data()), static_cast<gint>(bytes.size()));
          }, +[](GtkClipboard*, gpointer data) { delete static_cast<std::string*>(data); }, bytes.get()))
          throw Error("CLIPBOARD_ERROR", "Cannot write binary clipboard data");
        bytes.release(); gtk_clipboard_set_can_store(clipboard, &target, 1); respond({{"result", true}}); return;
      }
      if (method == "ReaWeb_ClipboardReadBinary") {
        struct BinaryRead { LinuxChannel* channel; std::string token; };
        auto type = "ReaWebAPI.Binary:" + args.at(0).get<std::string>();
        gtk_clipboard_request_contents(clipboard, gdk_atom_intern(type.c_str(), FALSE), +[](GtkClipboard*, GtkSelectionData* selection, gpointer data) {
          std::unique_ptr<BinaryRead> pending(static_cast<BinaryRead*>(data));
          const auto length = gtk_selection_data_get_length(selection);
          Json result = length < 0 ? Json() : length <= static_cast<int>(value_limit) ?
            encode_binary(reinterpret_cast<const char*>(gtk_selection_data_get_data(selection)), length) : Json();
          Json response = length <= static_cast<int>(value_limit) ? Json{{"result", result}} : Json{{"error", {{"code", "BUFFER_LIMIT"}, {"message", "Clipboard exceeds 16 MiB"}}}};
          try { pending->channel->send({{"op", "desktop-result"}, {"request", pending->token}, {"response", response}}); } catch (...) {}
        }, new BinaryRead{&channel, token}); return;
      }
      if (method == "ReaWeb_ClipboardWriteText") {
        const auto value = args.at(0).get<std::string>();
        if (value.size() > value_limit) throw Error("BUFFER_LIMIT", "Clipboard exceeds 16 MiB");
        gtk_clipboard_set_text(clipboard, value.c_str(), static_cast<int>(value.size()));
        gtk_clipboard_set_can_store(clipboard, nullptr, 0);
        respond({{"result", true}}); return;
      }
      if (method != "ReaWeb_ClipboardReadText") throw Error("UNKNOWN_API", "Unknown desktop operation");
      struct Pending { LinuxChannel* channel; std::string token; };
      auto pending = new Pending{&channel, token};
      gtk_clipboard_request_text(clipboard, +[](GtkClipboard*, const gchar* text, gpointer data) {
        std::unique_ptr<Pending> p(static_cast<Pending*>(data));
        try {
          const std::string value = text ? text : "";
          const Json response = value.size() <= value_limit ? Json{{"result", value}} :
            Json{{"error", {{"code", "BUFFER_LIMIT"}, {"message", "Clipboard exceeds 16 MiB"}}}};
          p->channel->send({{"op", "desktop-result"}, {"request", p->token}, {"response", response}});
        } catch (...) { gtk_main_quit(); }
      }, pending);
    } catch (const Error& error) { respond({{"error", {{"code", error.code}, {"message", error.what()}}}}); }
    catch (const std::exception& error) { respond({{"error", {{"code", "HOST_ERROR"}, {"message", error.what()}}}}); }
  }
  void pump() {
    channel.pump([this](const Json& request) {
      const int id = request.at("id");
      const auto op = request.at("op").get<std::string>();
      if (op == "idle-pool") { idle.capacity(request.at("target").get<unsigned>()); }
      if (op == "idle") { idle.maintain([this] { return std::make_shared<GtkIdleView>(context); }); }
      if (op == "idle-pool" || op == "idle") {
        const auto state = idle.diagnostics();
        if (state != last_idle) { channel.send({{"op", "idle-pool"}, {"state", state}}); last_idle = state; }
        return;
      }
      if (op == "desktop") { desktop(request); return; }
      try {
        if (op == "open") {
          if (pages.size() >= 32 || pages.count(id)) throw std::runtime_error("WebKit window limit exceeded");
          pages.emplace(id, std::make_unique<Page>(channel, context, request, idle.take()));
        } else if (op == "close") pages.erase(id);
        else if (auto it = pages.find(id); it != pages.end()) it->second->command(request);
      } catch (const std::exception& error) { channel.send({{"id", id}, {"op", "error"}, {"error", error.what()}}); }
    });
  }
};
}
}
int main(int argc, char** argv) {
  if (argc != 2) return 1;
  // WSL's X11 path without DRI3 can spend a frame waiting for server-side
  // backing pixmaps. Keep the fallback local to this helper and respect overrides.
  if (!g_getenv("GDK_RENDERING")) {
    struct utsname system{};
    if (uname(&system) == 0 && (strstr(system.release, "microsoft") || strstr(system.release, "Microsoft"))) {
      if (auto display = XOpenDisplay(nullptr)) {
        int opcode = 0, event = 0, error = 0;
        const bool dri3 = XQueryExtension(display, "DRI3", &opcode, &event, &error);
        XCloseDisplay(display);
        if (!dri3) g_setenv("GDK_RENDERING", "image", FALSE);
      }
    }
  }
  if (!gtk_init_check(nullptr, nullptr) || !GDK_IS_X11_DISPLAY(gdk_display_get_default())) return 1;
  try {
    reaweb::Process process(argv[1]);
    gdk_event_handler_set(+[](GdkEvent* event, gpointer data) {
      auto self = static_cast<reaweb::Process*>(data);
      for (const auto& page : self->pages) if (page.second->frame_event(event)) return;
      gtk_main_do_event(event);
    }, &process, nullptr);
    // Read host commands as soon as the socket becomes readable. The timer
    // remains only as a retry path for non-blocking output and housekeeping.
    auto input_source = g_unix_fd_add_full(G_PRIORITY_HIGH, process.fd(),
      static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
      +[](gint, GIOCondition, gpointer data) -> gboolean {
        try { static_cast<reaweb::Process*>(data)->pump(); }
        catch (...) { gtk_main_quit(); }
        return G_SOURCE_CONTINUE;
      }, &process, nullptr);
    auto timer = g_timeout_add(8, +[](gpointer data) -> gboolean {
      try { static_cast<reaweb::Process*>(data)->pump(); }
      catch (...) { gtk_main_quit(); }
      return G_SOURCE_CONTINUE;
    }, &process);
    gtk_main();
    gdk_event_handler_set(+[](GdkEvent* event, gpointer) { gtk_main_do_event(event); }, nullptr, nullptr);
    g_source_remove(input_source);
    g_source_remove(timer);
    return 0;
  } catch (...) { return 1; }
}
