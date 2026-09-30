#pragma once
// Colour filter controller (issue #288). Applies the DWM colour effect (MagSetFullscreenColorEffect)
// through the Magnification runtime's owner thread, deduped so an unchanged filter costs one compare
// per tick. At 1x it holds its OWN runtime reference while a filter is on, because the effect only
// exists while a runtime lives, and a live runtime taxes every cursor change any app makes (so it is
// held only while it has to be). Windows clears the effect when the process dies
// (docs/COLOUR-FILTER-FINDINGS.md), so a crash cannot leave the screen filtered.
#include "color_matrix.h"
namespace wind {
class ColorFilterController {
public:
    // want: the matrix DWM should apply now (identity = no filter). needOwnHold: keep a runtime
    // alive ourselves (1x); when false the engine's runtime carries the effect (zoomed).
    void apply(const ColorMatrix& want, bool needOwnHold);
    // Identity, then release our reference. Idempotent.
    void shutdown();
private:
    bool holding_ = false;
    bool applied_ = false;          // lastApplied_ is live in the current runtime
    ColorMatrix lastApplied_{};
};
}  // namespace wind
