#include "linux_channel.hpp"
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <webkit2/webkit2.h>
#include <X11/Xlib.h>
#include <cstring>
#include <memory>

namespace reaweb {
namespace {
class Page {
  LinuxChannel& channel_;
  int id_;
  std::string uri_;
  GtkWidget* plug_ = nullptr;
  WebKitWebView* view_ = nullptr;
  WebKitUserContentManager* manager_ = nullptr;
  ::Window parent_ = 0;
  bool failed_ = false;
  void fail(const std::string& error) {
    if (!failed_) {
      failed_ = true;
      try { channel_.send({{"id", id_}, {"op", "error"}, {"error", error}}); }
      catch (...) { gtk_main_quit(); }
    }
  }
public:
  Page(LinuxChannel& channel, WebKitWebContext* context, const Json& request)
    : channel_(channel), id_(request.at("id").get<int>()), uri_(request.at("uri").get<std::string>()) {
    manager_ = webkit_user_content_manager_new();
    g_signal_connect(manager_, "script-message-received::reaweb", G_CALLBACK(+[](WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer data) {
      auto self = static_cast<Page*>(data);
      const auto uri = webkit_web_view_get_uri(self->view_);
      if (self->failed_ || !uri || !same_document(uri, self->uri_)) return;
      auto value = webkit_javascript_result_get_js_value(result);
      if (!jsc_value_is_string(value)) return;
      auto message = jsc_value_to_string(value);
      try {
        if (strlen(message) > message_limit) self->fail("JavaScript message limit exceeded");
        else self->channel_.send({{"id", self->id_}, {"op", "message"}, {"message", message}});
      } catch (const std::exception& error) { self->fail(error.what()); }
      g_free(message);
    }), this);
    if (!webkit_user_content_manager_register_script_message_handler(manager_, "reaweb")) {
      g_object_unref(manager_);
      throw std::runtime_error("Could not register WebKitGTK bridge");
    }
    const auto source = request.at("script").get<std::string>();
    auto script = webkit_user_script_new(source.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(manager_, script);
    webkit_user_script_unref(script);
    view_ = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context, "user-content-manager", manager_, nullptr));
    g_object_ref_sink(view_);
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
        if (!uri || !same_document(uri, self->uri_)) { webkit_policy_decision_ignore(decision); return TRUE; }
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
      if (event == WEBKIT_LOAD_STARTED) {
        auto self = static_cast<Page*>(data);
        try { self->channel_.send({{"id", self->id_}, {"op", "navigating"}}); }
        catch (const std::exception& error) { self->fail(error.what()); }
      }
    }), this);
    plug_ = gtk_plug_new(0);
    g_object_ref_sink(plug_);
    // GtkPlug emits delete-event when parked on the X11 root. The native host owns closing.
    g_signal_connect(plug_, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) -> gboolean { return TRUE; }), nullptr);
    gtk_container_add(GTK_CONTAINER(plug_), GTK_WIDGET(view_));
    gtk_widget_realize(plug_);
    webkit_web_view_load_uri(view_, uri_.c_str());
  }
  ~Page() {
    failed_ = true;
    webkit_user_content_manager_unregister_script_message_handler(manager_, "reaweb");
    g_signal_handlers_disconnect_by_data(manager_, this);
    g_signal_handlers_disconnect_by_data(view_, this);
    webkit_web_view_stop_loading(view_);
    webkit_web_inspector_close(webkit_web_view_get_inspector(view_));
    gtk_widget_destroy(plug_);
    g_object_unref(view_); g_object_unref(manager_); g_object_unref(plug_);
  }
  void command(const Json& request) {
    const auto op = request.at("op").get<std::string>();
    if (op == "eval") {
      const auto script = request.at("script").get<std::string>();
      webkit_web_view_evaluate_javascript(view_, script.c_str(), static_cast<gssize>(script.size()), nullptr, nullptr, nullptr, nullptr, nullptr);
    } else if (op == "devtools") {
      webkit_web_inspector_show(webkit_web_view_get_inspector(view_));
    } else if (op == "park") {
      // SWELL destroys its old X11 top-level when docking. Move out before that happens.
      gtk_widget_hide(plug_);
      auto display = gdk_x11_display_get_xdisplay(gtk_widget_get_display(plug_));
      XReparentWindow(display, gdk_x11_window_get_xid(gtk_widget_get_window(plug_)), DefaultRootWindow(display), 0, 0);
      XSync(display, False);
      parent_ = 0;
      channel_.send({{"id", id_}, {"op", "parked"}});
    } else if (op == "geometry") {
      const auto parent = request.at("parent").get<unsigned long>();
      const int x = request.at("x"), y = request.at("y");
      const int width = std::max(1, request.at("width").get<int>()), height = std::max(1, request.at("height").get<int>());
      auto display = gdk_x11_display_get_xdisplay(gtk_widget_get_display(plug_));
      auto xid = gdk_x11_window_get_xid(gtk_widget_get_window(plug_));
      gdk_x11_display_error_trap_push(gtk_widget_get_display(plug_));
      if (parent != parent_) { XReparentWindow(display, xid, parent, x, y); parent_ = parent; }
      gtk_window_resize(GTK_WINDOW(plug_), width, height);
      // A foreign REAPER/X11 parent is not a GtkSocket, so allocate the client
      // viewport explicitly instead of relying on GTK socket size negotiation.
      GtkAllocation allocation{0, 0, width, height};
      gtk_widget_size_allocate(plug_, &allocation);
      XMoveResizeWindow(display, xid, x, y, width, height);
      if (request.at("visible").get<bool>()) gtk_widget_show_all(plug_);
      else gtk_widget_hide(plug_);
      XFlush(display);
      gdk_x11_display_error_trap_pop_ignored(gtk_widget_get_display(plug_));
    } else if (op == "focus" && parent_) {
      gtk_widget_grab_focus(GTK_WIDGET(view_));
      auto display = gtk_widget_get_display(plug_);
      gdk_x11_display_error_trap_push(display);
      XSetInputFocus(gdk_x11_display_get_xdisplay(display), gdk_x11_window_get_xid(gtk_widget_get_window(plug_)), RevertToParent, CurrentTime);
      gdk_x11_display_error_trap_pop_ignored(display);
    }
  }
};
struct Process {
  LinuxChannel channel{3};
  WebKitWebContext* context = nullptr;
  std::map<int, std::unique_ptr<Page>> pages;
  explicit Process(const char* data) {
    auto cache = (fs::path(data) / "Cache").string();
    auto manager = webkit_website_data_manager_new("base-data-directory", data, "base-cache-directory", cache.c_str(), nullptr);
    const auto cookies = (fs::path(data) / "cookies.sqlite").string();
    webkit_cookie_manager_set_persistent_storage(webkit_website_data_manager_get_cookie_manager(manager),
      cookies.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    context = webkit_web_context_new_with_website_data_manager(manager);
    g_object_unref(manager);
    channel.send({{"op", "ready"}, {"protocol", 1}, {"version", REAWEB_VERSION}, {"browserVersion",
      std::to_string(webkit_get_major_version()) + "." + std::to_string(webkit_get_minor_version()) + "." + std::to_string(webkit_get_micro_version())}});
  }
  ~Process() { pages.clear(); g_object_unref(context); }
  void desktop(const Json& request) {
    const auto token = request.at("request").get<std::string>();
    auto respond = [&](Json response) { channel.send({{"op", "desktop-result"}, {"request", token}, {"response", response}}); };
    try {
      const auto method = request.at("method").get<std::string>();
      const auto& args = request.at("args");
      if (method == "ReaWeb_OpenExternal") {
        const auto url = args.at(0).get<std::string>(); validate_external_url(url);
        GError* error = nullptr;
        if (!gtk_show_uri_on_window(nullptr, url.c_str(), GDK_CURRENT_TIME, &error)) {
          std::string message = error ? error->message : "Cannot open external link";
          if (error) g_error_free(error);
          throw Error("EXTERNAL_OPEN_FAILED", message);
        }
        respond({{"result", true}}); return;
      }
      auto clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
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
      if (op == "desktop") { desktop(request); return; }
      try {
        if (op == "open") {
          if (pages.size() >= 32 || pages.count(id)) throw std::runtime_error("WebKit window limit exceeded");
          pages.emplace(id, std::make_unique<Page>(channel, context, request));
        } else if (op == "close") pages.erase(id);
        else if (auto it = pages.find(id); it != pages.end()) it->second->command(request);
      } catch (const std::exception& error) { channel.send({{"id", id}, {"op", "error"}, {"error", error.what()}}); }
    });
  }
};
}
}
int main(int argc, char** argv) {
  if (argc != 2 || !gtk_init_check(nullptr, nullptr) || !GDK_IS_X11_DISPLAY(gdk_display_get_default())) return 1;
  try {
    reaweb::Process process(argv[1]);
    auto timer = g_timeout_add(8, +[](gpointer data) -> gboolean {
      try { static_cast<reaweb::Process*>(data)->pump(); }
      catch (...) { gtk_main_quit(); }
      return G_SOURCE_CONTINUE;
    }, &process);
    gtk_main();
    g_source_remove(timer);
    return 0;
  } catch (...) { return 1; }
}
