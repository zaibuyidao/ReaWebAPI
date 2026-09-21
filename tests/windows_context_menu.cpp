#include <windows.h>
#include "platform/windows/win_context_menu.hpp"
#include "platform/windows/win_devtools.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
void pump(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  do {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (done()) return;
    Sleep(5);
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("Context menu test timed out");
}
std::vector<std::wstring> labels(ICoreWebView2ContextMenuItemCollection* items) {
  UINT32 count = 0; CHECK(SUCCEEDED(items->get_Count(&count)));
  std::vector<std::wstring> result;
  for (UINT32 i = 0; i < count; ++i) {
    ComPtr<ICoreWebView2ContextMenuItem> item; CHECK(SUCCEEDED(items->GetValueAtIndex(i, &item)));
    LPWSTR label = nullptr; CHECK(SUCCEEDED(item->get_Label(&label)));
    result.emplace_back(label ? label : L""); CoTaskMemFree(label);
  }
  return result;
}
std::wstring script(ICoreWebView2* view, const wchar_t* source) {
  bool done = false;
  std::wstring result;
  CHECK(SUCCEEDED(view->ExecuteScript(source, Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
    [&](HRESULT hr, LPCWSTR value) -> HRESULT { if (value) result = value; done = SUCCEEDED(hr); return S_OK; }).Get())));
  pump([&] { return done; }); return result;
}
void right_click(ICoreWebView2* view, int x, int y) {
  for (const auto type : {L"mousePressed", L"mouseReleased"}) {
    bool done = false;
    const auto params = std::wstring(L"{\"type\":\"") + type + L"\",\"button\":\"right\",\"clickCount\":1,\"x\":" +
      std::to_wstring(x) + L",\"y\":" + std::to_wstring(y) + L"}";
    CHECK(SUCCEEDED(view->CallDevToolsProtocolMethod(L"Input.dispatchMouseEvent", params.c_str(),
      Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([&](HRESULT hr, LPCWSTR) -> HRESULT {
        done = SUCCEEDED(hr); return S_OK;
      }).Get())));
    pump([&] { return done; });
  }
}
HWND inspector_for(const reaweb::WinDevTools* tools) {
  struct Search { const reaweb::WinDevTools* tools; HWND found = nullptr; } search{tools};
  EnumWindows([](HWND window, LPARAM data) -> BOOL {
    auto visit = [](HWND child, LPARAM data) -> BOOL {
      auto& search = *reinterpret_cast<Search*>(data);
      if (GetPropW(child, L"ReaWebAPI.DevTools.Owner") == search.tools) search.found = child;
      return TRUE;
    };
    visit(window, data); EnumChildWindows(window, visit, data); return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.found;
}
int main() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
  auto host = CreateWindowW(L"STATIC", L"ReaWebAPI context menu test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
    40, 40, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  try {
    ComPtr<ICoreWebView2Environment> environment;
    auto profile = std::filesystem::current_path() / ("context-menu-test-" + std::to_string(GetCurrentProcessId()));
    CHECK(SUCCEEDED(CreateCoreWebView2EnvironmentWithOptions(nullptr, profile.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([&](HRESULT hr, ICoreWebView2Environment* value) -> HRESULT {
        if (SUCCEEDED(hr)) environment = value; return S_OK;
      }).Get())));
    pump([&] { return environment != nullptr; });
    ComPtr<ICoreWebView2Controller> controller;
    CHECK(SUCCEEDED(environment->CreateCoreWebView2Controller(host,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([&](HRESULT hr, ICoreWebView2Controller* value) -> HRESULT {
        if (SUCCEEDED(hr)) controller = value; return S_OK;
      }).Get())));
    pump([&] { return controller != nullptr; });
    ComPtr<ICoreWebView2> view; CHECK(SUCCEEDED(controller->get_CoreWebView2(&view)));
    RECT bounds{}; GetClientRect(host, &bounds); controller->put_Bounds(bounds);
    ComPtr<ICoreWebView2_11> menus; CHECK(SUCCEEDED(view.As(&menus)));
    std::vector<std::wstring> defaults;
    int inspect_index = -1;
    EventRegistrationToken token{};
    menus->add_ContextMenuRequested(Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
      [&](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
        ComPtr<ICoreWebView2ContextMenuItemCollection> items; args->get_MenuItems(&items);
        defaults = labels(items.Get()); inspect_index = -1;
        for (UINT32 i = 0; i < defaults.size(); ++i) {
          ComPtr<ICoreWebView2ContextMenuItem> item; items->GetValueAtIndex(i, &item);
          LPWSTR name = nullptr; item->get_Name(&name);
          if (name && !wcscmp(name, L"inspect")) inspect_index = static_cast<int>(i);
          CoTaskMemFree(name);
        }
        return S_OK;
      }).Get(), &token);
    bool docked = false;
    int select = 0;
    int toggles = 0, requested = 0, page_events = 0, developer_requests = 0;
    std::unique_ptr<reaweb::WinDevTools> tools;
    tools = std::make_unique<reaweb::WinDevTools>(host, [&] {
      RECT rect{}; GetClientRect(host, &rect); controller->put_Bounds(tools->layout(rect));
    }, [&] { controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); });
    CHECK(SUCCEEDED(reaweb::install_window_menu(view.Get(), [&] { docked = !docked; ++toggles; }, [&] { return docked; },
      [&](reaweb::DevToolsAction action) { tools->perform(action); ++developer_requests; },
      [&] { return tools->menu_state(); })));
    std::string error;
    menus->add_ContextMenuRequested(Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
      [&](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
        args->put_Handled(TRUE);
        try {
          ComPtr<ICoreWebView2ContextMenuItemCollection> items; CHECK(SUCCEEDED(args->get_MenuItems(&items)));
          const auto current = labels(items.Get());
          auto expected = defaults;
          if (inspect_index >= 0) expected.erase(expected.begin() + inspect_index);
          CHECK(!defaults.empty() && current.size() == expected.size() + 4);
          CHECK(current.front() == (docked ? L"Undock from REAPER" : L"Dock in REAPER"));
          const auto state = tools->menu_state();
          CHECK(current[1] == (state.shown ? L"Hide DevTools" : L"Open DevTools"));
          CHECK(current[2] == (state.floating ? L"Embed DevTools" : L"Float DevTools"));
          ComPtr<ICoreWebView2ContextMenuItem> mode; items->GetValueAtIndex(2, &mode);
          BOOL enabled = FALSE; mode->get_IsEnabled(&enabled);
          CHECK(!!enabled == (!state.floating || state.embedded_supported));
          CHECK(std::vector<std::wstring>(current.begin() + 4, current.end()) == expected);
          ComPtr<ICoreWebView2ContextMenuItem> separator; items->GetValueAtIndex(3, &separator);
          COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND kind{}; separator->get_Kind(&kind);
          CHECK(kind == COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR);
          if (select >= 0) {
            ComPtr<ICoreWebView2ContextMenuItem> item; items->GetValueAtIndex(select, &item);
            INT32 command = 0; item->get_CommandId(&command); args->put_SelectedCommandId(command);
          }
        } catch (const std::exception& failure) { error = failure.what(); }
        ++requested; return S_OK;
      }).Get(), &token);
    view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
      [&](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*) -> HRESULT { ++page_events; return S_OK; }).Get(), &token);
    int navigations = 0;
    view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
      [&](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT { ++navigations; return S_OK; }).Get(), &token);
    view->NavigateToString(LR"HTML(<!doctype html><body style="margin:0"><a href="https://example.com" style="position:absolute;top:20px">Link</a>
      <input value="Editable text" style="position:absolute;top:80px"><p id="text" style="position:absolute;top:140px">Selected text</p>
      <script>window.retained=42;addEventListener('contextmenu',e=>{if(window.suppress)e.preventDefault();chrome.webview.postMessage('context');});</script>)HTML");
    pump([&] { return navigations == 1; });
    for (int i = 0; i < 2; ++i) {
      right_click(view.Get(), 300, 250); pump([&] { return requested == i + 1 && (!error.empty() || toggles == i + 1); });
      CHECK(error.empty());
    }
    CHECK(!docked);
    docked = true; select = -1;
    for (int y : {25, 90, 165}) {
      script(view.Get(), L"getSelection().selectAllChildren(document.getElementById('text'))");
      const auto before = requested;
      right_click(view.Get(), 20, y); pump([&] { return requested > before; }); CHECK(error.empty());
    }
    CHECK(toggles == 2);
    auto choose = [&](int index) {
      select = index; const auto before = developer_requests;
      right_click(view.Get(), 200, 250);
      pump([&] { return !error.empty() || developer_requests == before + 1; }); CHECK(error.empty());
    };
    auto visible = [&] { return tools->diagnostics()["visible"].get<bool>(); };
    choose(2); CHECK(tools->state()["mode"] == "floating" && !visible());
    choose(2); CHECK(tools->state()["mode"] == "embedded" && !visible());
    choose(1); CHECK(tools->menu_state().shown && !visible());
    choose(1); CHECK(!tools->menu_state().shown && !visible());
    choose(1);
    pump([&] { tools->tick(view.Get()); return visible(); });
    const auto inspector = inspector_for(tools.get()); CHECK(inspector && IsChild(host, inspector));
    for (int i = 0; i < 3; ++i) {
      choose(2); CHECK(tools->menu_state().floating && visible() && !IsChild(host, inspector));
      choose(2); CHECK(!tools->menu_state().floating && visible() && IsChild(host, inspector));
      choose(1); CHECK(!visible());
      choose(1); CHECK(visible() && inspector_for(tools.get()) == inspector);
    }
    choose(1); choose(2); CHECK(tools->menu_state().floating && !visible());
    choose(1); CHECK(visible() && inspector_for(tools.get()) == inspector);
    choose(2); CHECK(IsChild(host, inspector));
    CHECK(toggles == 2 && navigations == 1 && script(view.Get(), L"window.retained") == L"42");
    script(view.Get(), L"window.suppress=true");
    const auto before = requested, events = page_events;
    right_click(view.Get(), 300, 250); pump([&] { return page_events > events; });
    const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    pump([&] { return std::chrono::steady_clock::now() >= settle; });
    CHECK(requested == before && toggles == 2);
    CHECK(script(view.Get(), L"window.retained") == L"42");
    script(view.Get(), L"window.suppress=false");
    PostMessageW(inspector, WM_CLOSE, 0, 0);
    pump([&] { tools->tick(view.Get()); return !IsWindow(inspector); });
    tools.reset();
    // Use a separate host to exercise the DPI guard without changing WebView2's own controller parent.
    auto previous_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    auto mixed_host = CreateWindowW(L"STATIC", L"DevTools DPI fallback test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
      80, 80, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetThreadDpiAwarenessContext(previous_dpi);
    CHECK(mixed_host);
    {
      tools = std::make_unique<reaweb::WinDevTools>(mixed_host, [] {}, [] {});
      const auto saved = tools->state();
      choose(1);
      pump([&] { tools->tick(view.Get()); return visible(); });
      CHECK(tools->diagnostics()["mode"] == "floating" && tools->diagnostics()["embeddedSupported"] == false);
      CHECK(!tools->diagnostics()["fallbackReason"].get<std::string>().empty() && tools->state() == saved);
      choose(1); CHECK(!visible());
      choose(1); CHECK(visible());
      tools->perform(reaweb::DevToolsAction::Embed); CHECK(tools->state() == saved);
      CHECK(script(view.Get(), L"window.retained") == L"42");
      tools.reset();
    }
    DestroyWindow(mixed_host);
    controller->Close(); DestroyWindow(host);
    std::cout << "WebView2 context menu: dynamic DevTools actions, hidden mode preferences, retained inspector, docking, default actions, page cancellation and DPI fallback passed\n";
    return 0;
  } catch (const std::exception& error) { DestroyWindow(host); std::cerr << error.what() << '\n'; return 1; }
}
