#include <windows.h>
#include "platform/windows/win_context_menu.hpp"
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
int main() {
  SetProcessDPIAware();
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
    EventRegistrationToken token{};
    menus->add_ContextMenuRequested(Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
      [&](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
        ComPtr<ICoreWebView2ContextMenuItemCollection> items; args->get_MenuItems(&items);
        defaults = labels(items.Get()); return S_OK;
      }).Get(), &token);
    bool docked = false, select = true;
    int toggles = 0, requested = 0, page_events = 0;
    CHECK(SUCCEEDED(reaweb::install_dock_menu(view.Get(), [&] { docked = !docked; ++toggles; }, [&] { return docked; })));
    std::string error;
    menus->add_ContextMenuRequested(Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
      [&](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
        args->put_Handled(TRUE);
        try {
          ComPtr<ICoreWebView2ContextMenuItemCollection> items; CHECK(SUCCEEDED(args->get_MenuItems(&items)));
          const auto current = labels(items.Get());
          CHECK(!defaults.empty() && current.size() == defaults.size() + 2);
          CHECK(current.front() == (docked ? L"Undock from REAPER" : L"Dock in REAPER"));
          CHECK(std::vector<std::wstring>(current.begin() + 2, current.end()) == defaults);
          ComPtr<ICoreWebView2ContextMenuItem> separator; items->GetValueAtIndex(1, &separator);
          COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND kind{}; separator->get_Kind(&kind);
          CHECK(kind == COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR);
          if (select) {
            ComPtr<ICoreWebView2ContextMenuItem> item; items->GetValueAtIndex(0, &item);
            INT32 command = 0; item->get_CommandId(&command); args->put_SelectedCommandId(command);
          }
        } catch (const std::exception& failure) { error = failure.what(); }
        ++requested; return S_OK;
      }).Get(), &token);
    view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
      [&](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*) -> HRESULT { ++page_events; return S_OK; }).Get(), &token);
    bool loaded = false;
    view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
      [&](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT { loaded = true; return S_OK; }).Get(), &token);
    view->NavigateToString(LR"HTML(<!doctype html><body style="margin:0"><a href="https://example.com" style="position:absolute;top:20px">Link</a>
      <input value="Editable text" style="position:absolute;top:80px"><p id="text" style="position:absolute;top:140px">Selected text</p>
      <script>window.retained=42;addEventListener('contextmenu',e=>{if(window.suppress)e.preventDefault();chrome.webview.postMessage('context');});</script>)HTML");
    pump([&] { return loaded; });
    for (int i = 0; i < 2; ++i) {
      right_click(view.Get(), 300, 250); pump([&] { return requested == i + 1 && (!error.empty() || toggles == i + 1); });
      CHECK(error.empty());
    }
    CHECK(!docked);
    docked = true; select = false;
    for (int y : {25, 90, 165}) {
      script(view.Get(), L"getSelection().selectAllChildren(document.getElementById('text'))");
      const auto before = requested;
      right_click(view.Get(), 20, y); pump([&] { return requested > before; }); CHECK(error.empty());
    }
    CHECK(toggles == 2);
    script(view.Get(), L"window.suppress=true");
    const auto before = requested, events = page_events;
    right_click(view.Get(), 300, 250); pump([&] { return page_events > events; });
    const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    pump([&] { return std::chrono::steady_clock::now() >= settle; });
    CHECK(requested == before && toggles == 2);
    CHECK(script(view.Get(), L"window.retained") == L"42");
    controller->Close(); DestroyWindow(host);
    std::cout << "WebView2 context menu: labels, ordering, default actions, callback, external state and page cancellation passed\n";
    return 0;
  } catch (const std::exception& error) { DestroyWindow(host); std::cerr << error.what() << '\n'; return 1; }
}
