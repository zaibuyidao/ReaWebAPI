#include "platform/linux/linux_overlay.hpp"
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/shape.h>
#include <iostream>
#include <chrono>
#include <thread>
using namespace reaweb;
#define CHECK(v) do { if (!(v)) throw std::runtime_error(#v); } while (false)
int main(int argc, char** argv) {
  gtk_init(&argc, &argv);
  try {
    auto display = XOpenDisplay(nullptr);
    const auto root = DefaultRootWindow(display);
    auto main = XCreateSimpleWindow(display, root, 0, 0, 800, 600, 0, 0, 0x123456);
    auto host = XCreateSimpleWindow(display, root, 100, 100, 400, 300, 0, 0, 0x654321);
    auto floating = XCreateSimpleWindow(display, root, 600, 100, 100, 100, 0, 0, 0x654321);
    XSelectInput(display, main, ButtonPressMask | ButtonReleaseMask);
    XSelectInput(display, host, ButtonPressMask | ButtonReleaseMask);
    XSetTransientForHint(display, host, main); XSetTransientForHint(display, floating, main);
    XMapWindow(display, main); XMapWindow(display, host); XMapWindow(display, floating); XSync(display, False);
    std::string error;
    LinuxOverlay overlay({"overlay", "regions", {{20,20,80,80}}}, [&](std::string value) { error = value; });
    overlay.attach(host, main);
    const auto pump = [&] {
      auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
      do { g_main_context_iteration(nullptr, false); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
      while (std::chrono::steady_clock::now() < until);
      CHECK(error.empty());
    };
    pump(); XMoveWindow(display, floating, 1100, 100); XSync(display, False); pump(); CHECK(overlay.layer_active()); CHECK(overlay.mouse_active());
    auto shape = [&](int expected) {
      int count, order; auto rects = XShapeGetRectangles(display, host, ShapeInput, &count, &order);
      if (rects) XFree(rects); CHECK(count == expected);
    };
    shape(1);
    const auto received = [&](::Window target, int type) {
      bool found = false;
      while (XPending(display)) { XEvent event; XNextEvent(display, &event); if (event.type == type && event.xbutton.window == target) found = true; }
      return found;
    };
    ::Window child; int x, y; XTranslateCoordinates(display, host, root, 0, 0, &x, &y, &child);
    XTestFakeMotionEvent(display, -1, x+30, y+30, CurrentTime);
    CHECK(XTestFakeButtonEvent(display, 1, True, CurrentTime)); XSync(display, False); pump();
    CHECK(received(host, ButtonPress));
    overlay.mode("passthrough"); CHECK(!overlay.mouse_active()); shape(1);
    XTestFakeMotionEvent(display, -1, x+200, y+200, CurrentTime);
    XTestFakeButtonEvent(display, 1, False, CurrentTime); XSync(display, False); pump();
    CHECK(overlay.mouse_active()); shape(0); CHECK(received(host, ButtonRelease));
    XTestFakeButtonEvent(display, 1, True, CurrentTime); XSync(display, False); pump();
    XTestFakeButtonEvent(display, 1, False, CurrentTime); XSync(display, False); pump();
    CHECK(received(main, ButtonPress));
    overlay.mode("regions"); overlay.regions({{-10,-10,30,30}, {60,60,20,20}}); shape(2);
    overlay.scale(2); shape(2);
    overlay.regions({}); shape(0);
    overlay.mode("normal"); shape(1);
    auto second = XCreateSimpleWindow(display, root, 200, 200, 200, 200, 0, 0, 0);
    XSetTransientForHint(display, second, main); XMapWindow(display, second); XSync(display, False);
    LinuxOverlay other({"overlay", "normal", {}}, [&](std::string value) { error = value; }); other.attach(second, main); pump();
    const auto frame = [&](::Window window) {
      for (;;) { ::Window r,p,*c; unsigned n; CHECK(XQueryTree(display,window,&r,&p,&c,&n)); if(c)XFree(c); if(p==r) return window; window=p; }
    };
    const auto check_order = [&] {
      const auto base=frame(main), a=frame(host), b=frame(second), f=frame(floating);
      ::Window r,p,*c; unsigned n; CHECK(XQueryTree(display,root,&r,&p,&c,&n));
      unsigned bi=n,ai=n,oi=n,fi=n;
      for(unsigned i=0;i<n;++i) { if(c[i]==base)bi=i; if(c[i]==a)ai=i; if(c[i]==b)oi=i; if(c[i]==f)fi=i; }
      if(c)XFree(c); CHECK(bi<ai && bi<oi && ai<fi && oi<fi);
    };
    check_order();
    XRaiseWindow(display, main); XSync(display, False); pump(); check_order();
    XRaiseWindow(display, floating); XSync(display, False); pump(); check_order();
    overlay.attach(0, 0); XDestroyWindow(display, host); XSync(display, False); pump();
    other.attach(0, 0); XDestroyWindow(display, second); XDestroyWindow(display, floating); XDestroyWindow(display, main); XSync(display, False);
    std::cout << "PASS: X11 native input shape, gesture deferral, clipping, scale, multiple overlays and cleanup\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
