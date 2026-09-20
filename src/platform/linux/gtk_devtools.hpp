#pragma once
#include "platform/shared/devtools.hpp"
#include <X11/Xatom.h>

namespace reaweb {
// WebKit owns the inspector web view. Only its container changes on mode switches.
class GtkDevTools {
  WebKitWebInspector* inspector_;
  GtkWidget *page_, *paned_, *panel_, *floating_, *mode_, *inspector_view_ = nullptr;
  DevToolsPreferences prefs_;
  std::function<void(Json)> changed_;
  bool visible_ = false, pending_ = false, detaching_ = false, positioning_ = false;
  bool requested_ = false;
  bool shortcut_down_ = false;
  int allocated_width_ = 0;
  ::Window owner_ = 0;

  void report() {
    auto value = prefs_.state(); value["visible"] = visible_;
    try { changed_(std::move(value)); } catch (...) { /* Never unwind through GTK. */ }
  }
  static void move(GtkWidget* widget, GtkWidget* container, bool pane = false) {
    if (gtk_widget_get_parent(widget) == container) return;
    g_object_ref(widget);
    if (auto parent = gtk_widget_get_parent(widget)) gtk_container_remove(GTK_CONTAINER(parent), widget);
    if (container) {
      if (pane) gtk_paned_pack2(GTK_PANED(container), widget, FALSE, TRUE);
      else gtk_container_add(GTK_CONTAINER(container), widget);
    }
    g_object_unref(widget);
  }
  void position() {
    const auto width = gtk_widget_get_allocated_width(paned_);
    if (width <= 1) return;
    positioning_ = true;
    gtk_paned_set_position(GTK_PANED(paned_), static_cast<int>(width * (1.0 - prefs_.width_ratio)));
    positioning_ = false;
    allocated_width_ = width;
  }
  void present(bool floating) {
    prefs_.floating = floating;
    gtk_widget_hide(panel_);
    move(panel_, floating ? floating_ : paned_, !floating);
    gtk_button_set_label(GTK_BUTTON(mode_), floating ? "Dock right" : "Float DevTools");
    gtk_widget_set_no_show_all(panel_, FALSE);
    gtk_widget_show_all(panel_);
    gtk_widget_set_no_show_all(panel_, TRUE);
    visible_ = true; pending_ = false;
    if (floating) {
      gtk_widget_show(floating_);
      owner(owner_);
      gtk_window_present(GTK_WINDOW(floating_));
    } else {
      gtk_widget_hide(floating_);
      position();
    }
    if (inspector_view_) gtk_widget_grab_focus(inspector_view_);
    report();
  }
  void opened() {
    auto view = GTK_WIDGET(webkit_web_inspector_get_web_view(inspector_));
    if (!view) return;
    if (view != inspector_view_) {
      inspector_view_ = view;
      move(view, panel_);
      gtk_box_set_child_packing(GTK_BOX(panel_), view, TRUE, TRUE, 0, GTK_PACK_START);
    }
    const bool show = !pending_ || requested_;
    pending_ = false;
    if (show) present(detaching_ ? true : prefs_.floating);
    else hide();
    detaching_ = false;
  }
  void hide() {
    visible_ = false;
    gtk_widget_hide(panel_); gtk_widget_hide(floating_);
    gtk_widget_grab_focus(page_);
    report();
  }
  void connect_keys(GtkWidget* widget) {
    g_signal_connect(widget, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer data) -> gboolean {
      auto self = static_cast<GtkDevTools*>(data);
      auto mods = event->state & gtk_accelerator_get_default_mod_mask();
      if (gdk_keyval_to_lower(event->keyval) != GDK_KEY_i || mods != (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        return FALSE;
      }
      if (!self->shortcut_down_) { self->shortcut_down_ = true; self->toggle(); }
      return TRUE;
    }), this);
    g_signal_connect(widget, "key-release-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer data) -> gboolean {
      if (gdk_keyval_to_lower(event->keyval) == GDK_KEY_i) static_cast<GtkDevTools*>(data)->shortcut_down_ = false;
      return FALSE;
    }), this);
  }
public:
  GtkDevTools(WebKitWebView* view, GtkWidget* plug, std::function<void(Json)> changed)
    : inspector_(webkit_web_view_get_inspector(view)), page_(GTK_WIDGET(view)), changed_(std::move(changed)) {
    paned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_wide_handle(GTK_PANED(paned_), TRUE);
    gtk_paned_pack1(GTK_PANED(paned_), page_, TRUE, TRUE);
    gtk_container_add(GTK_CONTAINER(plug), paned_);
    panel_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(panel_);
    // Host geometry updates must not reveal a hidden inspector via show_all().
    gtk_widget_set_no_show_all(panel_, TRUE);
    auto toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    mode_ = gtk_button_new_with_label("Float DevTools");
    auto close = gtk_button_new_with_label("Hide DevTools");
    gtk_box_pack_start(GTK_BOX(toolbar), mode_, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel_), toolbar, FALSE, FALSE, 0);
    floating_ = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(floating_);
    gtk_window_set_title(GTK_WINDOW(floating_), "ReaWebAPI DevTools");
    gtk_window_set_default_size(GTK_WINDOW(floating_), 800, 600);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(floating_), TRUE);
    g_signal_connect(floating_, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
      static_cast<GtkDevTools*>(data)->hide(); return TRUE;
    }), this);
    g_signal_connect(mode_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      auto self = static_cast<GtkDevTools*>(data); self->present(!self->prefs_.floating);
    }), this);
    g_signal_connect(close, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<GtkDevTools*>(data)->hide(); }), this);
    g_signal_connect(paned_, "size-allocate", G_CALLBACK(+[](GtkWidget*, GtkAllocation* allocation, gpointer data) {
      auto self = static_cast<GtkDevTools*>(data);
      if (allocation->width != self->allocated_width_ && self->visible_ && !self->prefs_.floating) self->position();
    }), this);
    g_signal_connect(paned_, "notify::position", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
      auto self = static_cast<GtkDevTools*>(data);
      const auto width = gtk_widget_get_allocated_width(self->paned_);
      if (self->positioning_ || !self->visible_ || self->prefs_.floating || width <= 1 || width != self->allocated_width_) return;
      self->prefs_.width_ratio = std::clamp(1.0 - static_cast<double>(gtk_paned_get_position(GTK_PANED(self->paned_))) / width, 0.2, 0.8);
      self->position(); self->report();
    }), this);
    g_signal_connect(inspector_, "open-window", G_CALLBACK(+[](WebKitWebInspector*, gpointer data) -> gboolean {
      static_cast<GtkDevTools*>(data)->opened(); return TRUE;
    }), this);
    g_signal_connect(inspector_, "attach", G_CALLBACK(+[](WebKitWebInspector*, gpointer data) -> gboolean {
      auto self = static_cast<GtkDevTools*>(data); self->prefs_.floating = false; self->detaching_ = false; self->opened(); return TRUE;
    }), this);
    g_signal_connect(inspector_, "detach", G_CALLBACK(+[](WebKitWebInspector*, gpointer data) -> gboolean {
      // Also emitted during native close; commit floating only on open-window.
      static_cast<GtkDevTools*>(data)->detaching_ = true; return TRUE;
    }), this);
    g_signal_connect(inspector_, "bring-to-front", G_CALLBACK(+[](WebKitWebInspector*, gpointer data) -> gboolean {
      auto self = static_cast<GtkDevTools*>(data); self->opened(); return TRUE;
    }), this);
    g_signal_connect(inspector_, "closed", G_CALLBACK(+[](WebKitWebInspector*, gpointer data) {
      auto self = static_cast<GtkDevTools*>(data);
      if (self->inspector_view_) move(self->inspector_view_, nullptr);
      self->inspector_view_ = nullptr; self->pending_ = false; self->detaching_ = false; self->hide();
    }), this);
    connect_keys(plug); connect_keys(floating_);
  }
  ~GtkDevTools() {
    g_signal_handlers_disconnect_by_data(inspector_, this);
    g_signal_handlers_disconnect_by_data(gtk_widget_get_toplevel(paned_), this);
    g_signal_handlers_disconnect_by_data(paned_, this);
    webkit_web_inspector_close(inspector_);
    gtk_widget_destroy(floating_); g_object_unref(floating_);
    gtk_widget_destroy(panel_); g_object_unref(panel_);
  }
  void toggle() {
    if (visible_ || (pending_ && requested_)) {
      // Let an in-flight open finish, then hide its view instead of racing close/reopen.
      requested_ = false;
      hide();
    } else open();
  }
  void open() {
    requested_ = true;
    if (inspector_view_) present(prefs_.floating);
    else if (!pending_) { pending_ = true; webkit_web_inspector_show(inspector_); }
  }
  void restore(const Json& state) {
    prefs_.restore(state);
    if (visible_) present(prefs_.floating);
    else report();
  }
  void owner(::Window parent) {
    owner_ = parent;
    if (!gtk_widget_get_realized(floating_)) return;
    auto display = gtk_widget_get_display(floating_);
    gdk_x11_display_error_trap_push(display);
    auto xdisplay = gdk_x11_display_get_xdisplay(display);
    auto xid = gdk_x11_window_get_xid(gtk_widget_get_window(floating_));
    if (parent) XSetTransientForHint(xdisplay, xid, parent);
    else XDeleteProperty(xdisplay, xid, XA_WM_TRANSIENT_FOR);
    gdk_x11_display_error_trap_pop_ignored(display);
  }
};
}
