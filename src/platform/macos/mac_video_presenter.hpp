#pragma once
#include "runtime/video_presenter.hpp"
#import <WebKit/WebKit.h>

namespace reaweb {
class MacVideoPresenter : public VideoPresenter {
public:
  virtual void update_view() = 0;
};
bool mac_video_presenter_available();
std::shared_ptr<MacVideoPresenter> make_mac_video_presenter(WKWebView*, const Json&);
}
