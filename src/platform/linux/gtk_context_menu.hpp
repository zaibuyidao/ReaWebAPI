#pragma once
#include <webkit2/webkit2.h>
#include <functional>

namespace reaweb {
class GtkDockMenu {
  WebKitWebView* view_;
  GSimpleAction* action_;
  std::function<void()> toggle_;
  bool docked_ = false;
public:
  GtkDockMenu(WebKitWebView* view, std::function<void()> toggle) : view_(view),
      action_(g_simple_action_new("reaweb-dock", nullptr)), toggle_(std::move(toggle)) {
    g_signal_connect(action_, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
      auto self = static_cast<GtkDockMenu*>(data);
      if (self->toggle_) self->toggle_();
    }), this);
    g_signal_connect(view_, "context-menu", G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu* menu,
        GdkEvent*, WebKitHitTestResult*, gpointer data) -> gboolean {
      auto self = static_cast<GtkDockMenu*>(data);
      // WebKitGTK can emit an empty proposed menu after DOM preventDefault().
      if (!self->toggle_ || !webkit_context_menu_get_n_items(menu)) return FALSE;
      webkit_context_menu_prepend(menu, webkit_context_menu_item_new_separator());
      webkit_context_menu_prepend(menu, webkit_context_menu_item_new_from_gaction(G_ACTION(self->action_),
        self->docked_ ? "Undock from REAPER" : "Dock in REAPER", nullptr));
      return FALSE;
    }), this);
  }
  ~GtkDockMenu() {
    g_signal_handlers_disconnect_by_data(view_, this);
    g_signal_handlers_disconnect_by_data(action_, this);
    g_object_unref(action_);
  }
  void set_docked(bool docked) { docked_ = docked; }
};
}
