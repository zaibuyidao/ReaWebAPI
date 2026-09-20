#include "platform/linux/gtk_context_menu.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
void until(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  do {
    while (gtk_events_pending()) gtk_main_iteration();
    if (done()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("Context menu test timed out");
}
std::string script(WebKitWebView* view, const char* source) {
  struct Result { bool done = false; std::string text; } result;
  webkit_web_view_evaluate_javascript(view, source, -1, nullptr, nullptr, nullptr, +[](GObject* source, GAsyncResult* async, gpointer data) {
    auto& result = *static_cast<Result*>(data);
    GError* error = nullptr;
    auto value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), async, &error);
    if (value) { auto text = jsc_value_to_string(value); result.text = text; g_free(text); g_object_unref(value); }
    if (error) { result.text = error->message; g_error_free(error); }
    result.done = true;
  }, &result);
  until([&] { return result.done; }); return result.text;
}
void right_click(WebKitWebView* view, double y) {
  for (auto type : {GDK_BUTTON_PRESS, GDK_BUTTON_RELEASE}) {
    auto event = gdk_event_new(type);
    event->button.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(GTK_WIDGET(view))));
    event->button.time = GDK_CURRENT_TIME; event->button.x = 20; event->button.y = y; event->button.button = 3;
    gdk_event_set_device(event, gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(GTK_WIDGET(view)))));
    gtk_widget_event(GTK_WIDGET(view), event); gdk_event_free(event);
  }
}
int main(int argc, char** argv) {
  if (!gtk_init_check(&argc, &argv)) return 1;
  try {
    auto window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 480);
    auto view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    webkit_settings_set_enable_developer_extras(webkit_web_view_get_settings(view), TRUE);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));
    gtk_widget_show_all(window);
    struct State {
      std::vector<WebKitContextMenuItem*> defaults;
      bool docked = false, select = true;
      int toggles = 0, requested = 0;
      std::string error;
    } state;
    g_signal_connect(view, "context-menu", G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu* menu, GdkEvent*, WebKitHitTestResult*, gpointer data) -> gboolean {
      auto& state = *static_cast<State*>(data); state.defaults.clear();
      for (auto item = webkit_context_menu_get_items(menu); item; item = item->next)
        state.defaults.push_back(WEBKIT_CONTEXT_MENU_ITEM(item->data));
      return FALSE;
    }), &state);
    {
      reaweb::GtkDockMenu docking(view, [&] { state.docked = !state.docked; ++state.toggles; });
      g_signal_connect(view, "context-menu", G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu* menu, GdkEvent*, WebKitHitTestResult*, gpointer data) -> gboolean {
        auto& state = *static_cast<State*>(data);
        try {
          if (state.defaults.empty()) {
            CHECK(!webkit_context_menu_get_n_items(menu));
            return FALSE;
          }
          CHECK(webkit_context_menu_get_n_items(menu) == state.defaults.size() + 2);
          auto item = webkit_context_menu_first(menu);
          G_GNUC_BEGIN_IGNORE_DEPRECATIONS
          CHECK(!g_strcmp0(gtk_action_get_label(webkit_context_menu_item_get_action(item)), state.docked ? "Undock from REAPER" : "Dock in REAPER"));
          G_GNUC_END_IGNORE_DEPRECATIONS
          CHECK(webkit_context_menu_item_is_separator(webkit_context_menu_get_item_at_position(menu, 1)));
          for (size_t i = 0; i < state.defaults.size(); ++i)
            CHECK(webkit_context_menu_get_item_at_position(menu, i + 2) == state.defaults[i]);
          if (state.select) g_action_activate(webkit_context_menu_item_get_gaction(item), nullptr);
        } catch (const std::exception& error) { state.error = error.what(); }
        ++state.requested; return TRUE;
      }), &state);
      webkit_web_view_load_html(view, R"HTML(<!doctype html><body style="margin:0"><a href="https://example.com" style="position:absolute;top:20px">Link</a>
        <input value="Editable text" style="position:absolute;top:80px"><p id="text" style="position:absolute;top:140px">Selected text</p>
        <script>window.retained=42;window.events=0;addEventListener('contextmenu',e=>{window.events++;if(window.suppress)e.preventDefault();});</script>)HTML", "http://localhost/");
      until([&] { return !webkit_web_view_is_loading(view); });
      for (int i = 0; i < 2; ++i) {
        docking.set_docked(state.docked); right_click(view, 250);
        until([&] { return state.requested == i + 1; }); CHECK(state.error.empty() && state.toggles == i + 1);
      }
      state.docked = true; state.select = false; docking.set_docked(true);
      for (double y : {25, 90, 165}) {
        script(view, "getSelection().selectAllChildren(document.getElementById('text'))");
        const auto before = state.requested;
        right_click(view, y); until([&] { return state.requested > before; }); CHECK(state.error.empty());
      }
      script(view, "window.suppress=true;window.events=0");
      const auto before = state.requested;
      right_click(view, 250);
      until([&] { return script(view, "window.events") == "1"; });
      const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
      until([&] { return std::chrono::steady_clock::now() >= settle; });
      CHECK(state.error.empty() && state.requested == before && state.toggles == 2);
      CHECK(script(view, "window.retained") == "42");
      g_signal_handlers_disconnect_by_data(view, &state);
    }
    gtk_widget_destroy(window);
    std::cout << "WebKitGTK context menu: labels, ordering, default actions, callback, external state and page cancellation passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
