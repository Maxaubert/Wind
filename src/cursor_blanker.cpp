#include "cursor_blanker.h"
namespace wind {

static const UINT kStandardIds[] = {
    32512, 32513, 32514, 32515, 32516, 32642, 32643,
    32644, 32645, 32646, 32648, 32649, 32650, 32651,
};

static HCURSOR CreateBlankCursor() {
    // 32x32 monochrome: AND mask all 1s (screen unchanged), XOR all 0s -> fully transparent.
    const int bytes = 32 * 32 / 8;
    BYTE andMask[bytes]; BYTE xorMask[bytes];
    memset(andMask, 0xFF, bytes);
    memset(xorMask, 0x00, bytes);
    return CreateCursor(nullptr, 0, 0, 32, 32, andMask, xorMask);
}

CursorBlanker::CursorBlanker() {
    // If a previous Wind was hard-killed while cursors were blanked, the desktop still has blank
    // shared cursors; reload the user's scheme FIRST or the blanks get captured as "originals".
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
    for (UINT id : kStandardIds) {
        HCURSOR shared = LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
        if (!shared) continue;
        HCURSOR copy = CopyCursor(shared);
        if (copy) originals_[shared] = copy;
    }
}

void CursorBlanker::blank() {
    if (blanked_) return;
    blanked_ = true;
    for (UINT id : kStandardIds) {
        HCURSOR blank = CreateBlankCursor();
        if (blank) SetSystemCursor(blank, id);   // SetSystemCursor takes ownership of 'blank'
    }
}

void CursorBlanker::restore() {
    if (!blanked_) return;
    blanked_ = false;
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
}
}
