#include "platform.hpp"
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

namespace reaweb {
namespace {
class WebKitWindow final : public Window {
  WindowOptions options_;
  std::string uri_;
  GtkWidget* window_ = nullptr;
  WebKitWebView* webview_ = nullptr;
  WebKitUserContentManager* manager_ = nullptr;
  bool closed_ = false;
public:
  WebKitWindow(WindowOptions options, WebKitWebContext* context) : options_(std::move(options)), uri_(file_uri(options_.entry)) {
    manager_ = webkit_user_content_manager_new();
    g_signal_connect(manager_, "script-message-received::reaweb", G_CALLBACK(+[](WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer data) {
      auto self = static_cast<WebKitWindow*>(data);
      if (self->closed_) return;
      const char* uri = webkit_web_view_get_uri(self->webview_);
      if (!uri || !same_document(uri, self->uri_)) return;
      auto value = webkit_javascript_result_get_js_value(result);
      if (!jsc_value_is_string(value)) return;
      auto message = jsc_value_to_string(value);
      self->options_.on_message(message);
      g_free(message);
    }), this);
    if (!webkit_user_content_manager_register_script_message_handler(manager_, "reaweb")) {
      g_object_unref(manager_); throw std::runtime_error("Could not register WebKitGTK bridge");
    }
    auto script = webkit_user_script_new(options_.script.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(manager_, script);
    webkit_user_script_unref(script);
    webview_ = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context, "user-content-manager", manager_, nullptr));
    g_object_ref_sink(webview_);
    auto settings = webkit_web_view_get_settings(webview_);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls(settings, FALSE);
    g_signal_connect(webview_, "decide-policy", G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer data) -> gboolean {
      auto self = static_cast<WebKitWindow*>(data);
      if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) { webkit_policy_decision_ignore(decision); return TRUE; }
      if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        auto action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        auto uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
        if (!uri || !same_document(uri, self->uri_)) { webkit_policy_decision_ignore(decision); return TRUE; }
      }
      return FALSE;
    }), this);
    g_signal_connect(webview_, "permission-request", G_CALLBACK(+[](WebKitWebView*, WebKitPermissionRequest* request, gpointer) -> gboolean {
      webkit_permission_request_deny(request); return TRUE;
    }), this);
    g_signal_connect(webview_, "web-process-terminated", G_CALLBACK(+[](WebKitWebView*, WebKitWebProcessTerminationReason, gpointer data) {
      auto self = static_cast<WebKitWindow*>(data); self->closed_ = true;
      self->options_.on_error("WebKitGTK content process terminated; reopen the tool");
    }), this);
    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(window_);
    gtk_window_set_title(GTK_WINDOW(window_), options_.title.c_str());
    gtk_window_set_default_size(GTK_WINDOW(window_), 860, 640);
    gtk_container_add(GTK_CONTAINER(window_), GTK_WIDGET(webview_));
    g_signal_connect(window_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) { static_cast<WebKitWindow*>(data)->closed_ = true; }), this);
    gtk_widget_show_all(window_);
    webkit_web_view_load_uri(webview_, uri_.c_str());
  }
  ~WebKitWindow() override {
    closed_ = true;
    webkit_user_content_manager_unregister_script_message_handler(manager_, "reaweb");
    g_signal_handlers_disconnect_by_data(manager_, this);
    g_signal_handlers_disconnect_by_data(webview_, this);
    g_signal_handlers_disconnect_by_data(window_, this);
    webkit_web_view_stop_loading(webview_);
    webkit_web_inspector_close(webkit_web_view_get_inspector(webview_));
    gtk_widget_destroy(window_);
    g_object_unref(webview_); g_object_unref(manager_); g_object_unref(window_);
  }
  void evaluate(const std::string& script) override {
    if (!closed_) webkit_web_view_evaluate_javascript(webview_, script.c_str(), static_cast<gssize>(script.size()), nullptr, nullptr, nullptr, nullptr, nullptr);
  }
  void devtools() override { webkit_web_inspector_show(webkit_web_view_get_inspector(webview_)); }
  bool closed() const override { return closed_; }
};
class GtkPlatform final : public Platform {
  WebKitWebContext* context_ = nullptr;
public:
  explicit GtkPlatform(const fs::path& data) {
    if (!gtk_init_check(nullptr, nullptr)) throw std::runtime_error("GTK initialization failed; a graphical desktop is required");
    const auto cache = (data / "Cache").string();
    auto manager = webkit_website_data_manager_new("base-data-directory", data.c_str(), "base-cache-directory", cache.c_str(), nullptr);
    context_ = webkit_web_context_new_with_website_data_manager(manager);
    g_object_unref(manager);
  }
  ~GtkPlatform() override { g_object_unref(context_); }
  std::shared_ptr<Window> open(WindowOptions options) override { return std::make_shared<WebKitWindow>(std::move(options), context_); }
  void pump() override {
    // REAPER owns the event loop. Never run gtk_main() inside the host.
    for (int n = 0; n < 32 && g_main_context_pending(nullptr); ++n) g_main_context_iteration(nullptr, FALSE);
  }
};
}
std::unique_ptr<Platform> make_platform(const fs::path& data) { return std::make_unique<GtkPlatform>(data); }
}
