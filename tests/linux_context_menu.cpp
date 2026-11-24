#include "platform/linux/gtk_context_menu.hpp"
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include "platform/linux/gtk_devtools.hpp"
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
    struct State {
      std::vector<WebKitContextMenuItem*> defaults;
      bool docked = false, select = true;
      int toggles = 0, requested = 0;
      std::vector<int> commands;
      std::string error;
      std::string app_name = "ReaGBA";
    } state;
    g_signal_connect(view, "context-menu", G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu* menu, GdkEvent*, WebKitHitTestResult*, gpointer data) -> gboolean {
      auto& state = *static_cast<State*>(data); state.defaults.clear();
      for (auto item = webkit_context_menu_get_items(menu); item; item = item->next)
        state.defaults.push_back(WEBKIT_CONTEXT_MENU_ITEM(item->data));
      return FALSE;
    }), &state);
    {
      reaweb::GtkDevTools tools(view, window, [](reaweb::Json) {});
      gtk_widget_show_all(window);
      reaweb::GtkDockMenu docking(view, [&] { state.docked = !state.docked; ++state.toggles; },
        [&](reaweb::DevToolsAction action) { tools.perform(action); }, [&] { return tools.menu_state(); }, [&](int command) { state.commands.push_back(command); });
      docking.set_app_name(state.app_name);
      auto check_popup = [&] {
        auto attached = gtk_menu_get_for_attach_widget(GTK_WIDGET(view));
        if (!attached) return false;
        auto menu = GTK_MENU(attached->data);
        if (!gtk_widget_get_visible(GTK_WIDGET(menu))) return false;
        auto children = gtk_container_get_children(GTK_CONTAINER(menu));
        CHECK(!state.defaults.empty() && g_list_length(children) == 7);
        const auto state_now = tools.menu_state();
        const std::string labels[] = {"Dock " + state.app_name + " in Docker", "Reload", "Find",
          state_now.shown ? "Hide DevTools" : "Open DevTools", state_now.floating ? "Embed DevTools" : "Float DevTools",
          "Open HTML Folder", "Close " + state.app_name};
        for (int i = 0; i < 7; ++i) {
          auto item = GTK_MENU_ITEM(g_list_nth_data(children, i));
          CHECK(gtk_menu_item_get_label(item) == labels[i]);
        }
        CHECK(!!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(children->data)) == state.docked);
        CHECK(!!gtk_widget_get_sensitive(GTK_WIDGET(g_list_nth_data(children, 4))) == state_now.mode_enabled());
        if (state.select) for (int i : {1,5,6,0})
          gtk_menu_item_activate(GTK_MENU_ITEM(g_list_nth_data(children, i)));
        gtk_menu_popdown(menu);
        g_list_free(children);
        ++state.requested;
        return true;
      };
      webkit_web_view_load_html(view, R"HTML(<!doctype html><body style="margin:0"><a href="https://example.com" style="position:absolute;top:20px">Link</a>
        <input value="Editable text" style="position:absolute;top:80px"><p id="text" style="position:absolute;top:140px">Selected text</p>
        <script>window.retained=42;window.events=0;addEventListener('contextmenu',e=>{window.events++;if(window.suppress)e.preventDefault();});</script>)HTML", "http://localhost/");
      until([&] { return !webkit_web_view_is_loading(view); });
      for (int i = 0; i < 2; ++i) {
        if (i) { state.app_name = "Rea&GBA 音"; docking.set_app_name(state.app_name); }
        docking.set_docked(state.docked); right_click(view, 250);
        until([&] { return check_popup() && state.requested == i + 1; }); CHECK(state.error.empty() && state.toggles == i + 1);
      }
      CHECK(state.commands == std::vector<int>({2,5,6,2,5,6}));
      state.docked = true; state.select = false; docking.set_docked(true);
      for (double y : {25, 90, 165}) {
        script(view, "getSelection().selectAllChildren(document.getElementById('text'))");
        const auto before = state.requested;
        right_click(view, y); until([&] { return check_popup() && state.requested > before; }); CHECK(state.error.empty());
      }
      const auto check_menu = [&] {
        const auto before = state.requested;
        right_click(view, 250); until([&] { return check_popup() && state.requested > before; }); CHECK(state.error.empty());
      };
      tools.perform(reaweb::DevToolsAction::Float);
      CHECK(!tools.menu_state().floating && !tools.menu_state().mode_enabled());
      tools.open(); until([&] { return tools.menu_state().shown; });
      for (int i = 0; i < 3; ++i) {
        check_menu(); tools.perform(reaweb::DevToolsAction::Float);
        CHECK(tools.menu_state().floating); check_menu();
        tools.perform(reaweb::DevToolsAction::Embed); CHECK(!tools.menu_state().floating);
        tools.perform(reaweb::DevToolsAction::Hide); check_menu();
        tools.open(); until([&] { return tools.menu_state().shown; });
      }
      tools.perform(reaweb::DevToolsAction::Hide);
      script(view, "window.suppress=true;window.events=0");
      const auto before = state.requested;
      right_click(view, 250);
      until([&] { return script(view, "window.events") == "1"; });
      const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
      until([&] { return std::chrono::steady_clock::now() >= settle; });
      CHECK(!check_popup() && state.error.empty() && state.requested == before && state.toggles == 2);
      CHECK(script(view, "window.retained") == "42");
      g_signal_handlers_disconnect_by_data(view, &state);
    }
    gtk_widget_destroy(window);
    std::cout << "WebKitGTK context menu: filtered actions, DevTools state, mode switching, docking and page cancellation passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
