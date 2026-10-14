#include "color_filter.h"
#include "mag_host.h"
#include "logging.h"
#include <windows.h>
#include <magnification.h>
#include <cstring>
namespace wind {

static bool WriteEffect(const ColorMatrix& m) {
    MAGCOLOREFFECT e{};
    static_assert(sizeof(e.transform) == sizeof(m.m), "MAGCOLOREFFECT is 5x5 floats");
    std::memcpy(e.transform, m.m, sizeof(m.m));
    return MagSetFullscreenColorEffect(&e) != FALSE;
}

void ColorFilterController::apply(const ColorMatrix& want, bool needOwnHold) {
    const bool identity = IsIdentity(want);
    // The effect lives in the runtime: if the runtime went away (the engine released its context
    // and we held none), whatever we applied is gone with it.
    if (!MagApiAlive()) applied_ = false;
    if (identity) needOwnHold = false;
    if (needOwnHold && !holding_) {
        holding_ = MagApiAcquire();
        if (holding_) applied_ = false;   // possibly a fresh runtime: re-apply
    }
    if (!MagApiAlive()) {
        // Nothing to write into yet (zoomed, before the engine's first write builds its context).
        // Retried next tick; identity needs nothing.
        return;
    }
    const bool alreadyThere = applied_ ? SameMatrix(want, lastApplied_) : identity;
    if (!alreadyThere) {
        const bool ok = WriteEffect(want);
        if (ok) { lastApplied_ = want; applied_ = true; }
        wind::Log(ok ? wind::LogLevel::Info : wind::LogLevel::Warn, "color", "effect %s (%s)",
                  identity ? "cleared" : "applied", ok ? "ok" : "FAILED");
    }
    if (!needOwnHold && holding_) {
        // Identity went out above before we let go, so releasing cannot strand a filtered screen.
        MagApiRelease();
        holding_ = false;
    }
}

void ColorFilterController::shutdown() {
    if (MagApiAlive() && applied_ && !IsIdentity(lastApplied_)) WriteEffect(IdentityColorMatrix());
    applied_ = false;
    if (holding_) { MagApiRelease(); holding_ = false; }
}
}  // namespace wind
