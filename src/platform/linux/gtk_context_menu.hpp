#pragma once
#include <webkit2/webkit2.h>
#include "platform/shared/devtools.hpp"

namespace reaweb {
class GtkDockMenu {
  WebKitWebView* view_;
  std::function<void()> toggle_;
  std::function<void(DevToolsAction)> devtools_;
  std::function<DevToolsMenuState()> state_;
  std::function<void(int)> window_action_;
  std::string app_name_;
  bool docked_ = false;
  GtkWidget* popup_ = nullptr;
  GtkWidget* find_ = nullptr;
  GtkWidget* entry_ = nullptr;

  void activate(int command) {
    if (command == 1) toggle_();
    else if (command == 3) devtools_(state_().shown ? DevToolsAction::Hide : DevToolsAction::Open);
    else if (command == 4) devtools_(state_().floating ? DevToolsAction::Embed : DevToolsAction::Float);
    else if (command == 7) show_find();
    else if (window_action_) window_action_(command);
  }
  void show_menu(GdkEvent* event) {
    if (popup_) gtk_widget_destroy(popup_);
    popup_ = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(popup_), GTK_WIDGET(view_), nullptr);
    auto append = [&](const std::string& text, int command, guint key = 0, bool enabled = true) {
      auto item = command == 1 ? gtk_check_menu_item_new_with_label(text.c_str()) : gtk_menu_item_new_with_label(text.c_str());
      if (command == 1) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), docked_);
      if (key) {
        auto label = GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(item)));
        gtk_accel_label_set_accel(label, key, command == 3 ? GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK) : GDK_CONTROL_MASK);
      }
      gtk_widget_set_sensitive(item, enabled);
      g_object_set_data(G_OBJECT(item), "reaweb-command", GINT_TO_POINTER(command));
      g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem* item, gpointer data) {
        static_cast<GtkDockMenu*>(data)->activate(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "reaweb-command")));
      }), this);
      gtk_menu_shell_append(GTK_MENU_SHELL(popup_), item);
    };
    if (toggle_) append("Dock " + app_name_ + " in Docker", 1);
    if (window_action_) { append("Reload", 2, GDK_KEY_r); append("Find", 7, GDK_KEY_f); }
    if (devtools_ && state_) {
      const auto state = state_();
      append(state.shown ? "Hide DevTools" : "Open DevTools", 3, GDK_KEY_i, state.available);
      append(state.floating ? "Embed DevTools" : "Float DevTools", 4, 0, state.mode_enabled());
    }
    if (window_action_) { append("Open HTML Folder", 5, 0, folder_enabled); append("Close " + app_name_, 6); }
    gtk_widget_show_all(popup_);
    gtk_menu_popup_at_pointer(GTK_MENU(popup_), event);
  }
public:
  bool folder_enabled = true;
  GtkDockMenu(WebKitWebView* view, std::function<void()> toggle, std::function<void(DevToolsAction)> devtools = {},
      std::function<DevToolsMenuState()> state = {}, std::function<void(int)> window_action = {})
    : view_(view), toggle_(std::move(toggle)), devtools_(std::move(devtools)), state_(std::move(state)), window_action_(std::move(window_action)) {
    g_signal_connect(view_, "context-menu", G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu* menu, GdkEvent* event,
        WebKitHitTestResult*, gpointer data) -> gboolean {
      if (!webkit_context_menu_get_n_items(menu)) return FALSE;
      static_cast<GtkDockMenu*>(data)->show_menu(event);
      return TRUE;
    }), this);
    g_signal_connect(view_, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer data) -> gboolean {
      auto self = static_cast<GtkDockMenu*>(data);
      const auto key = gdk_keyval_to_lower(event->keyval);
      if ((event->state & gtk_accelerator_get_default_mod_mask()) != GDK_CONTROL_MASK || (key != GDK_KEY_r && key != GDK_KEY_f)) return FALSE;
      self->activate(key == GDK_KEY_f ? 7 : 2);
      return TRUE;
    }), this);
  }
  ~GtkDockMenu() {
    if (popup_) gtk_widget_destroy(popup_);
    if (find_) gtk_widget_destroy(find_);
    g_signal_handlers_disconnect_by_data(view_, this);
  }
  void show_find() {
    if (!find_) {
      find_ = gtk_popover_new(GTK_WIDGET(view_));
      entry_ = gtk_search_entry_new();
      gtk_entry_set_placeholder_text(GTK_ENTRY(entry_), "Find in page");
      gtk_container_add(GTK_CONTAINER(find_), entry_);
      auto controller = webkit_web_view_get_find_controller(view_);
      g_signal_connect(entry_, "search-changed", G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
        auto find = WEBKIT_FIND_CONTROLLER(data);
        const auto text = gtk_entry_get_text(GTK_ENTRY(entry));
        if (*text) webkit_find_controller_search(find, text, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, G_MAXUINT);
        else webkit_find_controller_search_finish(find);
      }), controller);
      g_signal_connect_swapped(entry_, "activate", G_CALLBACK(webkit_find_controller_search_next), controller);
      g_signal_connect_swapped(entry_, "stop-search", G_CALLBACK(gtk_popover_popdown), find_);
      g_signal_connect_swapped(find_, "closed", G_CALLBACK(webkit_find_controller_search_finish), controller);
    }
    GdkRectangle corner{gtk_widget_get_allocated_width(GTK_WIDGET(view_)) - 16, 0, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(find_), &corner);
    gtk_widget_show_all(find_);
    gtk_popover_popup(GTK_POPOVER(find_));
    gtk_widget_grab_focus(entry_);
    gtk_editable_select_region(GTK_EDITABLE(entry_), 0, -1);
    g_signal_emit_by_name(entry_, "search-changed");
  }
  void set_docked(bool docked) { docked_ = docked; }
  void set_app_name(const std::string& name) { app_name_ = name; }
};
}
