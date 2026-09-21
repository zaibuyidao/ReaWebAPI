#pragma once
#include <wrl.h>
#include <WebView2.h>
#include <functional>
#include "platform/shared/devtools.hpp"

namespace reaweb {
inline HRESULT install_window_menu(ICoreWebView2* view, std::function<void()> toggle, std::function<bool()> is_docked,
                                   std::function<void(DevToolsAction)> devtools = {},
                                   std::function<DevToolsMenuState()> devtools_state = {}) {
  using Microsoft::WRL::ComPtr;
  using Microsoft::WRL::Callback;
  if (!toggle && !devtools) return S_OK;
  ComPtr<ICoreWebView2_11> menus;
  ComPtr<ICoreWebView2_2> view2;
  ComPtr<ICoreWebView2Environment> environment;
  ComPtr<ICoreWebView2Environment9> factory;
  if (FAILED(view->QueryInterface(IID_PPV_ARGS(&menus))) || FAILED(view->QueryInterface(IID_PPV_ARGS(&view2)))) return S_OK;
  HRESULT hr = view2->get_Environment(&environment);
  if (FAILED(hr)) return hr;
  if (FAILED(environment.As(&factory))) return S_OK;
  ComPtr<ICoreWebView2ContextMenuItem> dock, undock, separator, open, hide, floating, embedded;
  hr = factory->CreateContextMenuItem(L"Dock in REAPER", nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &dock);
  if (FAILED(hr)) return hr;
  hr = factory->CreateContextMenuItem(L"Undock from REAPER", nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &undock);
  if (FAILED(hr)) return hr;
  hr = factory->CreateContextMenuItem(L"", nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR, &separator);
  if (FAILED(hr)) return hr;
  auto selected = Callback<ICoreWebView2CustomItemSelectedEventHandler>(
    [toggle](ICoreWebView2ContextMenuItem*, IUnknown*) -> HRESULT { if (toggle) toggle(); return S_OK; });
  EventRegistrationToken token{};
  hr = dock->add_CustomItemSelected(selected.Get(), &token);
  if (FAILED(hr)) return hr;
  hr = undock->add_CustomItemSelected(selected.Get(), &token);
  if (FAILED(hr)) return hr;
  if (devtools && devtools_state) {
    const auto create = [&](const wchar_t* label, DevToolsAction action, ComPtr<ICoreWebView2ContextMenuItem>& item) {
      HRESULT result = factory->CreateContextMenuItem(label, nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &item);
      if (FAILED(result)) return result;
      return item->add_CustomItemSelected(Callback<ICoreWebView2CustomItemSelectedEventHandler>(
        [devtools, action](ICoreWebView2ContextMenuItem*, IUnknown*) -> HRESULT { devtools(action); return S_OK; }).Get(), &token);
    };
    hr = create(L"Open DevTools", DevToolsAction::Open, open); if (FAILED(hr)) return hr;
    hr = create(L"Hide DevTools", DevToolsAction::Hide, hide); if (FAILED(hr)) return hr;
    hr = create(L"Float DevTools", DevToolsAction::Float, floating); if (FAILED(hr)) return hr;
    hr = create(L"Embed DevTools", DevToolsAction::Embed, embedded); if (FAILED(hr)) return hr;
  }
  return menus->add_ContextMenuRequested(Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
    [dock, undock, separator, open, hide, floating, embedded, toggle, is_docked, devtools_state]
    (ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
      ComPtr<ICoreWebView2ContextMenuItemCollection> items;
      HRESULT hr = args->get_MenuItems(&items);
      if (FAILED(hr)) return hr;
      UINT32 count = 0;
      hr = items->get_Count(&count);
      if (FAILED(hr)) return hr;
      if (open) {
        // Route the native Inspect entry through the same host state as the shortcut/API.
        for (UINT32 i = 0; i < count; ++i) {
          ComPtr<ICoreWebView2ContextMenuItem> item;
          hr = items->GetValueAtIndex(i, &item); if (FAILED(hr)) return hr;
          LPWSTR name = nullptr; item->get_Name(&name);
          const bool inspect = name && !wcscmp(name, L"inspect"); CoTaskMemFree(name);
          if (!inspect) continue;
          hr = items->RemoveValueAtIndex(i); if (FAILED(hr)) return hr;
          --count; break;
        }
      }
      UINT32 index = 0;
      if (toggle) {
        hr = items->InsertValueAtIndex(index++, is_docked && is_docked() ? undock.Get() : dock.Get());
        if (FAILED(hr)) return hr;
      }
      if (open) {
        const auto state = devtools_state();
        hr = embedded->put_IsEnabled(state.embedded_supported); if (FAILED(hr)) return hr;
        hr = items->InsertValueAtIndex(index++, state.shown ? hide.Get() : open.Get()); if (FAILED(hr)) return hr;
        hr = items->InsertValueAtIndex(index++, state.floating ? embedded.Get() : floating.Get()); if (FAILED(hr)) return hr;
      }
      return index && count ? items->InsertValueAtIndex(index, separator.Get()) : S_OK;
    }).Get(), &token);
}
}
