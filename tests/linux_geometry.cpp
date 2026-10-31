#include "platform/linux/linux_geometry.hpp"
#include <stdexcept>
#include <iostream>

using Filter = int (*)(void*, void*, void*);
struct Native { int refs = 1; Filter filter = nullptr; void* data = nullptr; };
static int watches = 0, removes = 0;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
extern "C" void* g_object_ref(void* p) { ++static_cast<Native*>(p)->refs; return p; }
extern "C" void g_object_unref(void* p) { --static_cast<Native*>(p)->refs; }
extern "C" void gdk_window_add_filter(void* p, Filter filter, void* data) {
  auto& native = *static_cast<Native*>(p); CHECK(!native.filter);
  native.filter = filter; native.data = data; ++watches;
}
extern "C" void gdk_window_remove_filter(void* p, Filter filter, void* data) {
  auto& native = *static_cast<Native*>(p); CHECK(native.filter == filter && native.data == data);
  native.filter = nullptr; native.data = nullptr; ++removes;
}
int main() {
  Native first, replacement;
  {
    reaweb::LinuxGeometry geometry;
    auto revision = geometry.watch(&first);
    for (int n = 0; n < 1000; ++n) CHECK(geometry.watch(&first) == revision);
    CHECK(first.refs == 2 && watches == 1);
    for (int type : {Expose, MotionNotify, FocusIn, FocusOut}) {
      CHECK(first.filter(&type, nullptr, first.data) == 0);
      CHECK(geometry.watch(&first) == revision);
    }
    for (int type : {ConfigureNotify, ReparentNotify, MapNotify, UnmapNotify, PropertyNotify, GravityNotify, DestroyNotify}) {
      CHECK(first.filter(&type, nullptr, first.data) == 0);
      CHECK(geometry.watch(&first) != revision);
      revision = geometry.watch(&first);
    }
    geometry.invalidate(); CHECK(geometry.watch(&first) != revision);
    revision = geometry.watch(&first);
    CHECK(geometry.watch(&replacement) != revision);
    CHECK(first.refs == 1 && !first.filter && replacement.refs == 2);
    geometry.watch(nullptr); CHECK(replacement.refs == 1 && !replacement.filter);
    revision = geometry.watch(nullptr); CHECK(geometry.watch(nullptr) != revision);
    geometry.watch(&first);
  }
  CHECK(first.refs == 1 && !first.filter && watches == removes);
  std::cout << "Geometry invalidation, filter pass-through, reparenting and lifetime passed\n";
}
