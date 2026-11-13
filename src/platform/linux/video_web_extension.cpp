#include <webkit2/webkit-web-extension.h>
#include <gio/gunixfdlist.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cmath>
#include <map>
#include <memory>
#include <string>

namespace {
struct Mapping {
  void* data = MAP_FAILED;
  size_t bytes = 0;
  ~Mapping() { if (data != MAP_FAILED) munmap(data, bytes * 2); }
};
struct Document {
  std::map<std::string, std::shared_ptr<Mapping>> slots;
};
struct Page { std::shared_ptr<Document> document; };

JSCValue* copy(const char* token, int slot, double requested, gpointer data) {
  const auto document = *static_cast<std::shared_ptr<Document>*>(data);
  auto context = jsc_context_get_current();
  const auto found = document->slots.find(token ? token : "");
  if (found == document->slots.end()) return jsc_value_new_null(context);
  if (slot == -2) { document->slots.erase(found); return jsc_value_new_null(context); }
  if (slot == -1) return jsc_value_new_number(context, found->second->bytes);
  if (slot != 0 && slot != 1) return jsc_value_new_null(context);
  const auto& mapping = *found->second;
  if (std::isfinite(requested) && (requested < 0 || requested > mapping.bytes || std::floor(requested) != requested)) return jsc_value_new_null(context);
  const size_t count = std::isfinite(requested) && requested > 0 ? size_t(requested) : mapping.bytes;
  auto array = jsc_value_new_typed_array(context, JSC_TYPED_ARRAY_UINT8, count);
  gsize length = 0;
  auto bytes = jsc_value_typed_array_get_data(array, &length);
  if (!bytes || length != count) { g_object_unref(array); jsc_context_throw(context, "Unable to allocate video frame"); return jsc_value_new_null(context); }
  std::memcpy(bytes, static_cast<unsigned char*>(mapping.data) + slot * mapping.bytes, count);
  auto buffer = jsc_value_typed_array_get_buffer(array); g_object_unref(array); return buffer;
}

gboolean receive(WebKitWebPage* page, WebKitUserMessage* message, gpointer data) {
  if (std::strcmp(webkit_user_message_get_name(message), "reaweb-video-buffer")) return FALSE;
  auto state = static_cast<Page*>(data);
  const auto parameters = webkit_user_message_get_parameters(message);
  auto files = webkit_user_message_get_fd_list(message);
  if (!state->document || !parameters || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ssu)")) || !files) return TRUE;
  const char *token, *origin; guint bytes;
  g_variant_get(parameters, "(&s&su)", &token, &origin, &bytes);
  const std::string key(token), prefix = std::string(origin) + "/";
  const auto uri = webkit_web_page_get_uri(page);
  if (!uri || std::strncmp(uri, prefix.c_str(), prefix.size()) || key.size() != 64 ||
      key.find_first_not_of("0123456789abcdef") != std::string::npos || !bytes || bytes > 40u * 1024 * 1024 || state->document->slots.size() >= 64) return TRUE;
  const int fd = g_unix_fd_list_get(files, 0, nullptr);
  if (fd < 0) return TRUE;
  struct stat info{};
  const bool valid = !fstat(fd, &info) && info.st_size == off_t(size_t(bytes) * 2);
  auto mapping = std::make_shared<Mapping>(); mapping->bytes = bytes;
  if (valid) mapping->data = mmap(nullptr, size_t(bytes) * 2, PROT_READ, MAP_SHARED, fd, 0);
  ::close(fd);
  if (mapping->data != MAP_FAILED) state->document->slots[key] = std::move(mapping);
  return TRUE;
}
}

extern "C" G_MODULE_EXPORT void webkit_web_extension_initialize(WebKitWebExtension* extension) {
  g_signal_connect(extension, "page-created", G_CALLBACK(+[](WebKitWebExtension*, WebKitWebPage* page, gpointer) {
    auto state = new Page;
    g_object_set_data_full(G_OBJECT(page), "reaweb-video", state, +[](gpointer p) { delete static_cast<Page*>(p); });
    g_signal_connect(page, "user-message-received", G_CALLBACK(receive), state);
  }), nullptr);
  g_signal_connect(webkit_script_world_get_default(), "window-object-cleared", G_CALLBACK(+[](WebKitScriptWorld*, WebKitWebPage* page, WebKitFrame* frame, gpointer) {
    if (!webkit_frame_is_main_frame(frame)) return;
    auto state = static_cast<Page*>(g_object_get_data(G_OBJECT(page), "reaweb-video"));
    if (!state) return;
    if (state->document) state->document->slots.clear();
    state->document.reset();
    const auto uri = webkit_frame_get_uri(frame);
    if (!uri || std::strncmp(uri, "reaweb://", 9)) return;
    state->document = std::make_shared<Document>();
    auto context = webkit_frame_get_js_context(frame);
    auto function = jsc_value_new_function(context, "__reawebVideoCopy", G_CALLBACK(copy),
      new std::shared_ptr<Document>(state->document), +[](gpointer p) { delete static_cast<std::shared_ptr<Document>*>(p); }, JSC_TYPE_VALUE, 3, G_TYPE_STRING, G_TYPE_INT, G_TYPE_DOUBLE);
    auto global = jsc_context_get_global_object(context);
    jsc_value_object_define_property_data(global, "__reawebVideoCopy", JSCValuePropertyFlags(0), function);
    g_object_unref(global); g_object_unref(function);
    g_object_unref(context);
    webkit_web_page_send_message_to_view(page, webkit_user_message_new("reaweb-video-ready", nullptr), nullptr, nullptr, nullptr);
  }), nullptr);
}
