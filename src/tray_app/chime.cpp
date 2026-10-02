// The enable / disable chimes (issue #315): two tiny WAVE resources synthesized by
// tools/make_chimes.mjs, played asynchronously with no Windows system sound involved.
#include "tray_app.h"
#include "../resource.h"
#include <mmsystem.h>

namespace wind {
namespace TrayApp {

void PlayChime(bool enabled) {
    // SND_NODEFAULT: if the resource ever failed to load, stay silent instead of beeping.
    PlaySoundW(MAKEINTRESOURCEW(enabled ? IDR_CHIME_ON : IDR_CHIME_OFF), GetModuleHandleW(nullptr),
               SND_RESOURCE | SND_ASYNC | SND_NODEFAULT);
}

} // namespace TrayApp
} // namespace wind
