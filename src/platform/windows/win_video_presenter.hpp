#pragma once
#include "runtime/video_presenter.hpp"
#include <windows.h>

namespace reaweb {
class WinVideoHost {
public:
  virtual ~WinVideoHost() = default;
};
class WinVideoPresenter : public VideoPresenter {
public:
  virtual void update_view(const RECT& bounds, double css_scale, bool visible) = 0;
  virtual void detach_view() noexcept = 0;
};
std::shared_ptr<WinVideoHost> make_win_video_host(HWND);
std::shared_ptr<WinVideoPresenter> make_win_video_presenter(const std::shared_ptr<WinVideoHost>&, const Json&);
}
