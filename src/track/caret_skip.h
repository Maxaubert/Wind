// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#pragma once
// src/track/caret_skip.h - should a caret that moved within its focus be followed, or skipped?
//
// FocusRun::followCaret asks this after the MSAA swap and the trim, for a caret that is not the
// baseline and differs from the last one. Pure, no state changes: the caller logs the reason and
// updates the ghost state (NoteGhostHeld for a ghost, NoteFollowedCaret plus publish for None).
// Tests: tests/test_caret_skip.cpp.
#include "caret_rect.h"
#include "track_filter.h"

namespace wind {

enum class CaretSkip {
    None,             // follow it
    OutsideElement,   // a Gecko or UIA caret outside its focused element (#278, #561)
    ParkedTerminal,   // a TUI's parked cursor at the terminal's right edge (Claude Code)
    LineEndGhost,     // a line-end ghost (#387)
};

// rc: the caret; elem: the focused element's bounds (empty = unknown).
// geckoOrUia: the window is a Gecko browser, or the caret came from UIA (after the MSAA swap).
// terminalCandidate: a callable returning whether the window is a terminal whose input is not an
// xterm.js textarea. It is called only when the caret is not outside its element, so its window
// class lookup runs no more often than before.
// The checks run in priority order: outside, then parked, then ghost.
template <class TerminalCandidate>
inline CaretSkip ClassifyCaret(const TrackBox& rc, const TrackBox& elem, bool geckoOrUia,
                               TerminalCandidate&& terminalCandidate, const CaretGhostState& ghost) {
    if (geckoOrUia && !CaretInsideElement(rc, elem)) return CaretSkip::OutsideElement;
    if (terminalCandidate() &&
        IsParkedTerminalCaret((int)rc.l, (int)rc.t, (int)rc.r, (int)rc.b, (int)elem.l, (int)elem.r))
        return CaretSkip::ParkedTerminal;
    if (IsLineEndGhost((int)rc.l, (int)rc.t, (int)rc.b, ghost)) return CaretSkip::LineEndGhost;
    return CaretSkip::None;
}

}  // namespace wind
