#include <gtk/gtk.h>
#include <fstream>
#include <cstdlib>
#include <cstring>

// Keep the real helper and WebKit policies, replacing only the OS launcher.
static gboolean test_uri_open(GtkWindow*, const gchar* uri, guint32, GError** error) {
  std::ofstream(std::getenv("REAWEB_NAVIGATION_LOG"), std::ios::app) << uri << '\n';
  if (std::strcmp(uri, "https://example.com/fail") == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "No system handler");
    return FALSE;
  }
  return TRUE;
}
#define gtk_show_uri_on_window test_uri_open
#include "../src/platform/linux/linux_webkit.cpp"
#undef gtk_show_uri_on_window
