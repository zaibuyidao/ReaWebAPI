#pragma once
#include "core/core.hpp"
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

namespace reaweb {
class GtkNativeDrag {
  using UriList = std::unique_ptr<gchar*, decltype(&g_strfreev)>;
  using NativeString = std::unique_ptr<gchar, decltype(&g_free)>;
  GtkWidget* widget_;
  std::function<void(Json)> on_drop_, reply_;
  bool enabled_ = false, failed_ = false;
  Json payload_;
  GdkEvent* last_event_ = nullptr;
  GdkDragContext* source_ = nullptr;
  GdkDragContext* incoming_ = nullptr;
  int drop_x_ = 0, drop_y_ = 0;
  void finish(bool ok) {
    auto reply = std::move(reply_); payload_ = nullptr;
    if (source_) { g_object_unref(source_); source_ = nullptr; }
    if (reply) try { reply({{"result", ok}}); } catch (...) {}
  }
  GdkAtom target(GdkDragContext* context) {
    if (!(gdk_drag_context_get_actions(context) & GDK_ACTION_COPY)) return GDK_NONE;
    auto targets = gtk_target_list_new(nullptr, 0);
    gtk_target_list_add_uri_targets(targets, 1);
    gtk_target_list_add_text_targets(targets, 2);
    auto value = gtk_drag_dest_find_target(widget_, context, targets);
    gtk_target_list_unref(targets); return value;
  }
public:
  GtkNativeDrag(GtkWidget* widget, std::function<void(Json)> drop) : widget_(widget), on_drop_(std::move(drop)) {
    g_signal_connect(widget_, "event", G_CALLBACK(+[](GtkWidget*, GdkEvent* event, gpointer data) -> gboolean {
      auto self = static_cast<GtkNativeDrag*>(data);
      if (event->type == GDK_BUTTON_PRESS || event->type == GDK_MOTION_NOTIFY) {
        if (self->last_event_) gdk_event_free(self->last_event_);
        self->last_event_ = gdk_event_copy(event);
      }
      return FALSE;
    }), this);
    g_signal_connect(widget_, "drag-data-get", G_CALLBACK(+[](GtkWidget* widget, GdkDragContext*, GtkSelectionData* data, guint, guint, gpointer pointer) {
      auto self = static_cast<GtkNativeDrag*>(pointer);
      if (!self->reply_) return;
      g_signal_stop_emission_by_name(widget, "drag-data-get");
      try {
        if (!self->payload_.at("files").empty()) {
          std::vector<NativeString> owned;
          std::vector<gchar*> uris;
          for (const auto& path : self->payload_.at("files")) {
            owned.emplace_back(g_filename_to_uri(path.get<std::string>().c_str(), nullptr, nullptr), g_free);
            if (!owned.back()) throw Error("INVALID_PATH", "Cannot encode native file URI");
            uris.push_back(owned.back().get());
          }
          uris.push_back(nullptr); gtk_selection_data_set_uris(data, uris.data());
        } else {
          const auto text = self->payload_.at("text").get<std::string>();
          gtk_selection_data_set_text(data, text.c_str(), static_cast<int>(text.size()));
        }
      } catch (const std::exception& error) { self->failed_ = true; g_warning("ReaWebAPI drag: %s", error.what()); }
    }), this);
    g_signal_connect(widget_, "drag-failed", G_CALLBACK(+[](GtkWidget*, GdkDragContext*, GtkDragResult, gpointer data) -> gboolean {
      auto self = static_cast<GtkNativeDrag*>(data);
      if (!self->reply_) return FALSE;
      self->failed_ = true; return TRUE;
    }), this);
    g_signal_connect(widget_, "drag-end", G_CALLBACK(+[](GtkWidget* widget, GdkDragContext* context, gpointer data) {
      auto self = static_cast<GtkNativeDrag*>(data);
      if (!self->reply_) return;
      g_signal_stop_emission_by_name(widget, "drag-end");
      self->finish(!self->failed_ && gdk_drag_context_get_selected_action(context) == GDK_ACTION_COPY);
    }), this);
    g_signal_connect(widget_, "drag-motion", G_CALLBACK(+[](GtkWidget*, GdkDragContext* context, gint, gint, guint time, gpointer data) -> gboolean {
      auto self = static_cast<GtkNativeDrag*>(data);
      if (!self->enabled_) return FALSE;
      gdk_drag_status(context, self->target(context) != GDK_NONE ? GDK_ACTION_COPY : static_cast<GdkDragAction>(0), time); return TRUE;
    }), this);
    g_signal_connect(widget_, "drag-drop", G_CALLBACK(+[](GtkWidget* widget, GdkDragContext* context, gint x, gint y, guint time, gpointer data) -> gboolean {
      auto self = static_cast<GtkNativeDrag*>(data);
      if (!self->enabled_) return FALSE;
      const auto atom = self->target(context);
      if (atom == GDK_NONE || self->incoming_) { gtk_drag_finish(context, FALSE, FALSE, time); return TRUE; }
      self->incoming_ = GDK_DRAG_CONTEXT(g_object_ref(context)); self->drop_x_ = x; self->drop_y_ = y;
      gtk_drag_get_data(widget, context, atom, time); return TRUE;
    }), this);
    g_signal_connect(widget_, "drag-data-received", G_CALLBACK(+[](GtkWidget* widget, GdkDragContext* context, gint, gint, GtkSelectionData* selection, guint, guint time, gpointer pointer) {
      auto self = static_cast<GtkNativeDrag*>(pointer);
      if (context != self->incoming_) return;
      g_signal_stop_emission_by_name(widget, "drag-data-received");
      bool accepted = false;
      try {
        Json files = Json::array(); std::string text;
        const auto size = gtk_selection_data_get_length(selection);
        if (self->enabled_ && size >= 0 && static_cast<size_t>(size) <= value_limit) {
          auto uris = gtk_selection_data_get_uris(selection);
          if (uris) {
            UriList uri_owner(uris, g_strfreev);
            for (size_t i = 0; uris[i]; ++i) {
              if (i >= 256) throw Error("BUFFER_LIMIT", "At most 256 dropped files");
              gchar* hostname = nullptr;
              auto path = g_filename_from_uri(uris[i], &hostname, nullptr);
              NativeString path_owner(path, g_free);
              NativeString host_owner(hostname, g_free);
              const bool local = !hostname || !*hostname || !g_ascii_strcasecmp(hostname, "localhost");
              if (path && local) files.push_back(std::string(path));
            }
          } else {
            auto value = gtk_selection_data_get_text(selection);
            if (value) { text = reinterpret_cast<const char*>(value); g_free(value); }
          }
          if (!files.empty() || !text.empty()) {
            const double zoom = webkit_web_view_get_zoom_level(WEBKIT_WEB_VIEW(widget));
            self->on_drop_({{"files", files}, {"text", text}, {"x", self->drop_x_ / zoom}, {"y", self->drop_y_ / zoom}});
            accepted = true;
          }
        }
      } catch (const std::exception& error) { g_warning("ReaWebAPI drop: %s", error.what()); }
      gtk_drag_finish(context, accepted, FALSE, time);
      g_object_unref(self->incoming_); self->incoming_ = nullptr;
    }), this);
  }
  ~GtkNativeDrag() {
    enabled_ = false;
    if (source_) gtk_drag_cancel(source_);
    finish(false);
    g_signal_handlers_disconnect_by_data(widget_, this);
    if (incoming_) { gtk_drag_finish(incoming_, FALSE, FALSE, GDK_CURRENT_TIME); g_object_unref(incoming_); }
    if (last_event_) gdk_event_free(last_event_);
  }
  void enabled(bool value) { enabled_ = value; }
  void start(const Json& payload, std::function<void(Json)> reply) {
    if (reply_) throw Error("DRAG_BUSY", "A native drag is already active");
    auto display = gtk_widget_get_display(widget_);
    auto pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
    GdkModifierType state{};
    auto window = gtk_widget_get_window(widget_);
    if (!window || !pointer) throw Error("DRAG_GESTURE_REQUIRED", "A visible pointer gesture is required");
    gdk_device_get_state(pointer, window, nullptr, &state);
    if (!(state & GDK_BUTTON1_MASK)) throw Error("DRAG_GESTURE_REQUIRED", "Hold the left mouse button while starting the native drag");
    auto targets = gtk_target_list_new(nullptr, 0);
    if (!payload.at("files").empty()) gtk_target_list_add_uri_targets(targets, 1); else gtk_target_list_add_text_targets(targets, 2);
    payload_ = payload; reply_ = std::move(reply); failed_ = false;
    auto context = gtk_drag_begin_with_coordinates(widget_, targets, GDK_ACTION_COPY, 1, last_event_, -1, -1);
    gtk_target_list_unref(targets);
    if (!context) { reply_ = {}; payload_ = nullptr; throw Error("DRAG_FAILED", "GTK could not start native drag"); }
    source_ = GDK_DRAG_CONTEXT(g_object_ref(context)); gtk_drag_set_icon_default(context);
  }
};
}
