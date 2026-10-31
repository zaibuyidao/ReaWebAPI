#include "platform/linux/linux_geometry.hpp"
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <stdexcept>
#include <iostream>

#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
static void pump() {
  for (int i = 0; i < 30; ++i) { while (gtk_events_pending()) gtk_main_iteration(); g_usleep(10000); }
}
int main(int argc, char** argv) {
  gdk_set_allowed_backends("x11");
  CHECK(gtk_init_check(&argc, &argv));
  CHECK(GDK_IS_X11_DISPLAY(gdk_display_get_default()));
  reaweb::LinuxGeometry geometry;
  for (int pass = 0; pass < 3; ++pass) {
    auto widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(widget), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(widget), 320, 240);
    gtk_widget_show_all(widget); pump();
    auto native = gtk_widget_get_window(widget);
    auto revision = geometry.watch(native);
    pump(); revision = geometry.watch(native);
    auto display = gdk_x11_display_get_xdisplay(gdk_window_get_display(native));
    auto xid = gdk_x11_window_get_xid(native);
    for (int step = 0; step < 4; ++step) {
      // Move through X11, not the cache's explicit invalidation path.
      XMoveResizeWindow(display, xid, 100 + step * 40, 120 + step * 30, 360 + step * 20, 260 + step * 15);
      XFlush(display); pump();
      CHECK(geometry.watch(native) != revision);
      revision = geometry.watch(native);
      CHECK(gdk_window_get_width(native) == 360 + step * 20);
      CHECK(gdk_window_get_height(native) == 260 + step * 15);
    }
    gtk_widget_hide(widget); pump(); CHECK(geometry.watch(native) != revision);
    revision = geometry.watch(native);
    gtk_widget_show(widget); pump(); CHECK(geometry.watch(native) != revision);
    gtk_widget_destroy(widget); pump();
    geometry.watch(nullptr);
  }
  std::cout << "PASS: real X11 move/resize, hide/show and native-window recreation\n";
}
