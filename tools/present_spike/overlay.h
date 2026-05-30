#pragma once
#include <windows.h>

namespace spike {

// The three present configurations under test (issue #69).
//  Blt          - blt-model DXGI_SWAP_EFFECT_DISCARD on a WS_EX_LAYERED window, paced by DwmFlush
//                 (the current shipping path / baseline).
//  DcompNoLayer - flip-model CreateSwapChainForComposition via DComp on a WS_EX_NOREDIRECTIONBITMAP
//                 (NOT layered) window. The #11 config: smooth, but clicks were eaten.
//  DcompLayered - the same flip-model DComp visual but on a WS_EX_LAYERED window. The #24 config:
//                 clicks worked, but reverted on (unmeasured) background lag.
enum class PresentMode { Blt, DcompNoLayer, DcompLayered };

// A fullscreen present overlay in one of the three configs. Renders a recognizable semi-transparent
// frame; present is config-specific. All D3D/DXGI/DComp headers stay inside overlay.cpp.
class Overlay {
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool init(PresentMode mode);
    // Render + present one frame. `phase` shifts the fill color so successive frames differ (so the
    // compositor sees real change during the pacing test). `paceWithDwmFlush` (blt only) presents
    // immediately then blocks on DwmFlush, mirroring the shipping default. Returns false on failure.
    bool renderFrame(double phase, bool paceWithDwmFlush);
    void shutdown();
    PresentMode mode() const;

private:
    struct State;
    State* s_;
};

} // namespace spike
