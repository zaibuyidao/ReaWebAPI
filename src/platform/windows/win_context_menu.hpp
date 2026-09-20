#pragma once
#include <wrl.h>
#include <WebView2.h>
#include <functional>

namespace reaweb {
inline HRESULT install_dock_menu(ICoreWebView2* view, std::function<void()> toggle, std::function<bool()> is_docked) {
  using Microsoft::WRL::ComPtr;
  using Microsoft::WRL::Callback;
  if (!toggle) return S_OK;
  ComPtr<ICoreWebView2_11> menus;
  ComPtr<ICoreWebView2_2> view2;
  ComPtr<ICoreWebView2Environment> environment;
  ComPtr<ICoreWebView2Environment9> factory;
  if (FAILED(view->QueryInterface(IID_PPV_ARGS(&menus))) || FAILED(view->QueryInterface(IID_PPV_ARGS(&view2)))) return S_OK;
  HRESULT hr = view2->get_Environment(&environment);
  if (FAILED(hr)) return hr;
  if (FAILED(environment.As(&factory))) return S_OK;
  ComPtr<ICoreWebView2ContextMenuItem> dock, undock, separator;
  hr = factory->CreateContextMenuItem(L"Dock in REAPER", nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &dock);
  if (FAILED(hr)) return hr;
  hr = factory->CreateContextMenuItem(L"Undock from REAPER", nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &undock);
  if (FAILED(hr)) return hr;
  hr = factory->CreateContextMenuItem(L"", nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR, &separator);
  if (FAILED(hr)) return hr;
  auto selected = Callback<ICoreWebView2CustomItemSelectedEventHandler>(
    [toggle](ICoreWebView2ContextMenuItem*, IUnknown*) -> HRESULT { toggle(); return S_OK; });
  EventRegistrationToken token{};
  hr = dock->add_CustomItemSelected(selected.Get(), &token);
  if (FAILED(hr)) return hr;
  hr = undock->add_CustomItemSelected(selected.Get(), &token);
  if (FAILED(hr)) return hr;
  return menus->add_ContextMenuRequested(Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
    [dock, undock, separator, is_docked](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
      ComPtr<ICoreWebView2ContextMenuItemCollection> items;
      HRESULT hr = args->get_MenuItems(&items);
      if (FAILED(hr)) return hr;
      UINT32 count = 0;
      hr = items->get_Count(&count);
      if (FAILED(hr)) return hr;
      hr = items->InsertValueAtIndex(0, is_docked && is_docked() ? undock.Get() : dock.Get());
      if (FAILED(hr)) return hr;
      return count ? items->InsertValueAtIndex(1, separator.Get()) : S_OK;
    }).Get(), &token);
}
}
