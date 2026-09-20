#include "platform/linux/linux_icon.hpp"
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <iostream>
using namespace reaweb;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
int main(int argc, char** argv) {
  try {
    gdk_set_allowed_backends("x11");
    CHECK(gtk_init_check(&argc, &argv));
    const std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><rect width='16' height='16' fill='red' fill-opacity='.5'/></svg>";
    auto images = render_icon({".svg", {svg.begin(), svg.end()}}, {16, 32});
    LinuxIcon icon;
    for (int n = 0; n < 3; ++n) {
      auto window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_widget_realize(window);
      auto native = gtk_widget_get_window(window);
      if (!n) icon.set(native, images); else icon.refresh(native);
      CHECK(icon.last_error.empty());
      auto display = gdk_x11_display_get_xdisplay(gdk_window_get_display(native));
      Atom actual; int format; unsigned long count, remaining; unsigned char* bytes = nullptr;
      CHECK(XGetWindowProperty(display, gdk_x11_window_get_xid(native), XInternAtom(display, "_NET_WM_ICON", False),
        0, 2048, False, XA_CARDINAL, &actual, &format, &count, &remaining, &bytes) == Success);
      CHECK(bytes && format == 32 && count == 16 * 16 + 32 * 32 + 4);
      auto values = reinterpret_cast<unsigned long*>(bytes);
      CHECK(values[0] == 16 && values[1] == 16 && values[258] == 32 && values[259] == 32);
      CHECK((values[2] & 0xffffff) == 0xff0000 && (values[2] >> 24) >= 127 && (values[2] >> 24) <= 128);
      XFree(bytes);
      icon.refresh(nullptr); gtk_widget_destroy(window);
    }
    std::cout << "Linux native window icon, alpha and reparent restoration passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
