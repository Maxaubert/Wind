#pragma once
#include <windows.h>
namespace wind {
class CompositionPin {
public:
    bool create();
    void assert_();
    void hide();
    void destroy();
private:
    HWND hwnd_ = nullptr;
    bool visible_ = false;
};

// MPO buster (issue #191): a FULLSCREEN alpha-1 click-through layered ghost shown during
// transform GAME sessions on MPO-enabled machines. A shown fullscreen layered window over a
// game demotes it off its hardware overlay/independent-flip plane (the parking law, issue
// #90/#154) - and off the plane there is no 16-bit translation field to overflow, so the
// #148 corner TDR cannot fire and the pan walls can lift. Alpha 1, NEVER 0 (DWM drops fully
// transparent windows - see CompositionPin); UNBANDED (it only needs to cover the unbanded
// game; banding would reopen the #162 Snipping trade for nothing); never presents, so the
// stale-frame law has no content to flash. Sibling of CompositionPin by design: same window
// recipe, monitor-sized.
class MpoGhost {
public:
    bool create(int x, int y, int w, int h);
    void assert_();                        // show + keep top-of-band (500ms cadence at the caller)
    void hide();
    void destroy();
    // Fail-closed evidence for the pan-wall lift: the ghost is verifiably SHOWN at fullscreen
    // bounds and has been up long enough for the plane demotion to settle (~350ms). The walls
    // lift ONLY while this is true; any failure keeps or restores them the same tick.
    bool settled(unsigned long long nowMs) const;
private:
    HWND hwnd_ = nullptr;
    bool visible_ = false;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    unsigned long long shownAtMs_ = 0;
};
}
