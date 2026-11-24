#pragma once
#include <wrl.h>
#include <WebView2.h>
#include <functional>

namespace reaweb {
inline HRESULT install_window_menu(ICoreWebView2* view, std::function<void(POINT)> show) {
  Microsoft::WRL::ComPtr<ICoreWebView2_11> menus;
  if (FAILED(view->QueryInterface(IID_PPV_ARGS(&menus)))) return S_OK;
  EventRegistrationToken token{};
  return menus->add_ContextMenuRequested(Microsoft::WRL::Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
    [show](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
      POINT point{};
      auto hr = args->get_Location(&point);
      if (FAILED(hr)) return hr;
      hr = args->put_Handled(TRUE);
      if (SUCCEEDED(hr)) show(point);
      return hr;
    }).Get(), &token);
}

}
