#include "platform/linux/linux_icon.hpp"
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <iostream>
using namespace reaweb;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
std::vector<unsigned long> property(GdkWindow* window, const char* name) {
  auto display = gdk_x11_display_get_xdisplay(gdk_window_get_display(window));
  Atom type; int format; unsigned long count, remaining; unsigned char* bytes = nullptr;
  CHECK(XGetWindowProperty(display, gdk_x11_window_get_xid(window), XInternAtom(display, name, False),
    0, 65536, False, AnyPropertyType, &type, &format, &count, &remaining, &bytes) == Success);
  CHECK(!remaining && (!count || format == 32));
  std::vector<unsigned long> values;
  if (count) values.assign(reinterpret_cast<unsigned long*>(bytes), reinterpret_cast<unsigned long*>(bytes) + count);
  if (bytes) XFree(bytes);
  return values;
}
int main(int argc, char** argv) {
  try {
    gdk_set_allowed_backends("x11");
    CHECK(gtk_init_check(&argc, &argv));
    const std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><rect width='16' height='16' fill='red' fill-opacity='.5'/></svg>";
    auto images = render_icon({".svg", {svg.begin(), svg.end()}}, {16, 32});
    auto fallback = gdk_pixbuf_new_from_data(images[0].rgba.data(), GDK_COLORSPACE_RGB, TRUE, 8, 16, 16, 64, nullptr, nullptr);
    CHECK(fallback); gtk_window_set_default_icon(fallback); g_object_unref(fallback);
    LinuxIcon icon;
    for (int n = 0; n < 3; ++n) {
      auto window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_widget_realize(window);
      auto native = gtk_widget_get_window(window);
      auto display = gdk_x11_display_get_xdisplay(gdk_window_get_display(native));
      const auto property_count = [&] {
        Atom type; int bits; unsigned long length, left; unsigned char* data = nullptr;
        CHECK(XGetWindowProperty(display, gdk_x11_window_get_xid(native), XInternAtom(display, "_NET_WM_ICON", False),
          0, 2048, False, XA_CARDINAL, &type, &bits, &length, &left, &data) == Success);
        if (data) XFree(data);
        return length;
      };
      if (!n) {
        gtk_widget_show(window);
        while (gtk_events_pending()) gtk_main_iteration();
        CHECK(property_count() != 0);
        icon.set_visible(native, false);
      } else icon.refresh(native);
      CHECK(icon.last_error.empty() && property_count() == 0);
      GdkWMDecoration startup_decorations{}; CHECK(gdk_window_get_decorations(native, &startup_decorations));
      CHECK((startup_decorations & GDK_DECOR_ALL) && (startup_decorations & GDK_DECOR_MENU));
      if (!n) icon.set(native, images);
      gtk_widget_show(window);
      while (gtk_events_pending()) gtk_main_iteration();
      CHECK(property_count() == 0);
      icon.set_visible(native, true);
      Atom actual; int format; unsigned long count, remaining; unsigned char* bytes = nullptr;
      CHECK(XGetWindowProperty(display, gdk_x11_window_get_xid(native), XInternAtom(display, "_NET_WM_ICON", False),
        0, 2048, False, XA_CARDINAL, &actual, &format, &count, &remaining, &bytes) == Success);
      CHECK(bytes && format == 32 && count == 16 * 16 + 32 * 32 + 4);
      auto values = reinterpret_cast<unsigned long*>(bytes);
      CHECK(values[0] == 16 && values[1] == 16 && values[258] == 32 && values[259] == 32);
      CHECK((values[2] & 0xffffff) == 0xff0000 && (values[2] >> 24) >= 127 && (values[2] >> 24) <= 128);
      XFree(bytes);
      icon.set_visible(native, false);
      CHECK(property_count() == 0);
      GdkWMDecoration decorations{}; CHECK(gdk_window_get_decorations(native, &decorations));
      CHECK((decorations & GDK_DECOR_ALL) && (decorations & GDK_DECOR_MENU));
      gdk_window_set_decorations(native, GdkWMDecoration(GDK_DECOR_BORDER | GDK_DECOR_TITLE | GDK_DECOR_MENU));
      icon.refresh(native);
      CHECK(gdk_window_get_decorations(native, &decorations));
      CHECK(!(decorations & GDK_DECOR_MENU) && (decorations & GDK_DECOR_TITLE));
      icon.set(native, images); CHECK(property_count() == 0);
      icon.set_visible(native, true);
      CHECK(property_count() == 16 * 16 + 32 * 32 + 4);
      CHECK(gdk_window_get_decorations(native, &decorations));
      CHECK((decorations & GDK_DECOR_MENU) && (decorations & GDK_DECOR_TITLE));
      icon.set_visible(native, false);
      if (n == 2) {
        icon.clear(native); icon.refresh(native);
        icon.set_visible(native, true);
        bytes = nullptr;
        CHECK(XGetWindowProperty(display, gdk_x11_window_get_xid(native), XInternAtom(display, "_NET_WM_ICON", False),
          0, 2048, False, XA_CARDINAL, &actual, &format, &count, &remaining, &bytes) == Success);
        CHECK(count == 0);
        if (bytes) XFree(bytes);
      }
      icon.refresh(nullptr); gtk_widget_destroy(window);
    }
    auto host = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_widget_realize(host);
    auto native = gtk_widget_get_window(host);
    const auto original = property(native, "_NET_WM_ICON"), decorations = property(native, "_MOTIF_WM_HINTS");
    auto blue = images;
    for (auto& bitmap : blue) for (size_t i = 0; i < bitmap.rgba.size(); i += 4) { bitmap.rgba[i] = 0; bitmap.rgba[i+2] = 255; }
    {
      LinuxIcon first, second;
      first.refresh(native, true); CHECK(property(native, "_NET_WM_ICON") == original);
      first.set(native, blue, true); CHECK((property(native, "_NET_WM_ICON")[2] & 0xffffff) == 0x0000ff);
      first.set_visible(native, false, true); CHECK(property(native, "_NET_WM_ICON").empty());
      first.set(native, images, true); CHECK(property(native, "_NET_WM_ICON").empty());
      first.set_visible(native, true, true); CHECK((property(native, "_NET_WM_ICON")[2] & 0xffffff) == 0xff0000);
      second.set(native, blue, true);
      first.refresh(nullptr, true); CHECK((property(native, "_NET_WM_ICON")[2] & 0xffffff) == 0x0000ff);
      second.clear(native, true);
      CHECK(property(native, "_NET_WM_ICON") == original && property(native, "_MOTIF_WM_HINTS") == decorations);
      first.refresh(native, true);
    }
    CHECK(property(native, "_NET_WM_ICON") == original && property(native, "_MOTIF_WM_HINTS") == decorations);
    {
      LinuxIcon owner; owner.set(native, blue, true);
      gtk_widget_destroy(host); owner.refresh(nullptr, true);
    }
    std::cout << "Linux native window icon, alpha, reparent and shared Docker restoration passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
