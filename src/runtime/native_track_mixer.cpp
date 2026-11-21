#include "runtime/native_track_mixer.hpp"

#if !REAWEB_TRACK_WINDOWS_X64 && !REAWEB_TRACK_RENDER_CACHE
namespace reaweb {
bool NativeTrackMixer::initialize(const Host&, void*, Observer) { return false; }
void NativeTrackMixer::active(bool) noexcept {}
void NativeTrackMixer::stop() noexcept {}
}
#endif

namespace reaweb {
NativeTrackMixer::~NativeTrackMixer() { stop(); }
}
