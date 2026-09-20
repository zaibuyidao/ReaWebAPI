#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <webkit2/webkit2.h>
#include <X11/Xlib.h>
#include "platform/linux/gtk_devtools.hpp"
#include <iostream>
#include <thread>
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
using namespace reaweb;
void until(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  do {
    while (gtk_events_pending()) gtk_main_iteration();
    if (done()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("Inspector test timed out");
}
GtkWidget* button(GtkWidget* widget, const char* label) {
  if (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label)) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  auto children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget* found = nullptr;
  for (auto item = children; item && !found; item = item->next) found = button(GTK_WIDGET(item->data), label);
  g_list_free(children); return found;
}
void shortcut(GtkWidget* widget, bool release = false) {
  GdkEventKey event{}; event.type = release ? GDK_KEY_RELEASE : GDK_KEY_PRESS;
  event.keyval = GDK_KEY_I; event.state = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
  gboolean handled = FALSE;
  g_signal_emit_by_name(widget, release ? "key-release-event" : "key-press-event", &event, &handled);
  if (!release) CHECK(handled);
}
int main(int argc, char** argv) {
  if (!gtk_init_check(&argc, &argv)) return 1;
  try {
    auto window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 700);
    auto view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    webkit_settings_set_enable_developer_extras(webkit_web_view_get_settings(view), TRUE);
    Json state = Json::object();
    int navigations = 0;
    g_signal_connect(view, "load-changed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent event, gpointer data) {
      if (event == WEBKIT_LOAD_STARTED) ++*static_cast<int*>(data);
    }), &navigations);
    {
      GtkDevTools tools(view, window, [&](Json next) { state = next; });
      gtk_widget_show_all(window);
      webkit_web_view_load_html(view, "<!doctype html><h1>Inspector persistence test</h1><script>window.token='retained';console.log('retained console entry')</script>", "http://localhost/");
      until([&] { return navigations && !webkit_web_view_is_loading(view); });
      tools.open();
      until([&] { return state.value("visible", false); });
      auto inspector = webkit_web_view_get_inspector(view);
      auto inspector_view = GTK_WIDGET(webkit_web_inspector_get_web_view(inspector));
      CHECK(inspector_view);
      auto paned = gtk_bin_get_child(GTK_BIN(window)); CHECK(GTK_IS_PANED(paned));
      auto panel = gtk_paned_get_child2(GTK_PANED(paned)); CHECK(panel);
      CHECK(gtk_widget_get_parent(inspector_view) == panel && state["mode"] == "embedded");
      until([&] { return gtk_widget_get_allocated_width(inspector_view) > 100; });
      gtk_paned_set_position(GTK_PANED(paned), 720);
      until([&] { return state["widthRatio"].get<double>() > 0.39 && state["widthRatio"].get<double>() < 0.41; });
      gtk_paned_set_position(GTK_PANED(paned), 600);
      until([&] { return state["widthRatio"].get<double>() > 0.49; });
      const auto ratio = state["widthRatio"];
      gtk_window_resize(GTK_WINDOW(window), 1000, 700);
      until([&] { return gtk_widget_get_allocated_width(paned) == 1000; });
      CHECK(state["widthRatio"] == ratio);
      gtk_button_clicked(GTK_BUTTON(button(panel, "Float DevTools")));
      CHECK(state["mode"] == "floating" && gtk_widget_get_toplevel(inspector_view) != window);
      CHECK(webkit_web_inspector_get_web_view(inspector) == WEBKIT_WEB_VIEW_BASE(inspector_view));
      auto floating = gtk_widget_get_toplevel(inspector_view);
      shortcut(floating); CHECK(state["visible"] == false);
      shortcut(window); CHECK(state["visible"] == false); // Repeat after focus change.
      shortcut(window, true); shortcut(window); CHECK(state["visible"] == true);
      shortcut(floating, true);
      gtk_button_clicked(GTK_BUTTON(button(panel, "Dock right")));
      CHECK(state["mode"] == "embedded" && gtk_widget_get_toplevel(inspector_view) == window);
      CHECK(webkit_web_inspector_get_web_view(inspector) == WEBKIT_WEB_VIEW_BASE(inspector_view));
      tools.toggle(); gtk_widget_show_all(window); CHECK(!gtk_widget_get_visible(panel));
      tools.open(); CHECK(state["visible"] == true);
      webkit_web_inspector_close(inspector);
      until([&] { return state["visible"] == false; });
      CHECK(state["mode"] == "embedded" && state["widthRatio"] == ratio);
      tools.open(); until([&] { return state.value("visible", false); });
      CHECK(navigations == 1);
      // Reparenting and closing the inspector must preserve the inspected JS world.
      bool completed = false;
      webkit_web_view_evaluate_javascript(view, "window.token", -1, nullptr, nullptr, nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
          auto value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, nullptr);
          auto text = value ? jsc_value_to_string(value) : nullptr;
          *static_cast<bool*>(data) = text && !strcmp(text, "retained");
          g_free(text); if (value) g_object_unref(value);
        }, &completed);
      until([&] { return completed; });
    }
    gtk_widget_destroy(window);
    std::cout << "WebKitGTK DevTools: split, resize ratio, float/dock reuse, hide/show, native close and JS state passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
