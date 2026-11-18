// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "doctest.h"
#include "caret_skip.h"
using namespace wind;

namespace {
const auto kTerminal = [] { return true; };
const auto kNotTerminal = [] { return false; };
}

TEST_CASE("caret skip: a caret inside its element, in no terminal and no ghost, is followed") {
    const CaretGhostState g;
    CHECK(ClassifyCaret({ 1541, 1046, 1543, 1083 }, { 1520, 1022, 2372, 1108 }, true, kNotTerminal, g) == CaretSkip::None);
    CHECK(ClassifyCaret({ 1541, 1046, 1543, 1083 }, { 1520, 1022, 2372, 1108 }, false, kNotTerminal, g) == CaretSkip::None);
}

TEST_CASE("caret skip: a Gecko or UIA caret outside its element is skipped (#278, #561)") {
    const CaretGhostState g;
    const TrackBox input{ 1520, 1022, 2372, 1108 };                    // Prism dialog, field 2026-11-10
    CHECK(ClassifyCaret({ 2070, 1170, 2072, 1208 }, input, true, kNotTerminal, g) == CaretSkip::OutsideElement);
    // A Win32 caret in a non-Gecko window (geckoOrUia false) is not judged by its element.
    CHECK(ClassifyCaret({ 2070, 1170, 2072, 1208 }, input, false, kNotTerminal, g) == CaretSkip::None);
    // An unknown (empty) element trusts the caret.
    CHECK(ClassifyCaret({ 2070, 1170, 2072, 1208 }, { 0, 0, 0, 0 }, true, kNotTerminal, g) == CaretSkip::None);
}

TEST_CASE("caret skip: the element test keeps its 8 px slack") {
    const CaretGhostState g;
    const TrackBox elem{ 100, 100, 200, 140 };
    CHECK(ClassifyCaret({ 206, 110, 208, 130 }, elem, true, kNotTerminal, g) == CaretSkip::None);            // centre 207
    CHECK(ClassifyCaret({ 208, 110, 210, 130 }, elem, true, kNotTerminal, g) == CaretSkip::OutsideElement);  // centre 209
}

TEST_CASE("caret skip: a parked terminal cursor is skipped; xterm or a non-terminal disables it") {
    const CaretGhostState g;
    const TrackBox term{ 0, 0, 1000, 800 };
    const TrackBox parked{ 985, 100, 995, 120 };                       // one cell wide, at the right edge
    CHECK(ClassifyCaret(parked, term, true, kTerminal, g) == CaretSkip::ParkedTerminal);
    CHECK(ClassifyCaret(parked, term, false, kTerminal, g) == CaretSkip::ParkedTerminal);
    // The caller's terminal candidate is false for an xterm.js input or a non-terminal window.
    CHECK(ClassifyCaret(parked, term, true, kNotTerminal, g) == CaretSkip::None);
    // A caret mid-line in the terminal is followed.
    CHECK(ClassifyCaret({ 300, 100, 310, 120 }, term, true, kTerminal, g) == CaretSkip::None);
}

TEST_CASE("caret skip: outside beats parked, and the terminal check is not asked then") {
    const CaretGhostState g;
    const TrackBox term{ 0, 0, 1000, 800 };
    const TrackBox below{ 985, 900, 995, 920 };                        // parked column, below the element
    int asked = 0;
    const auto countingTerminal = [&] { ++asked; return true; };
    CHECK(ClassifyCaret(below, term, true, countingTerminal, g) == CaretSkip::OutsideElement);
    CHECK(asked == 0);
    CHECK(ClassifyCaret(below, term, false, countingTerminal, g) == CaretSkip::ParkedTerminal);
    CHECK(asked == 1);
}

TEST_CASE("caret skip: a line-end ghost is skipped (#387, Discord field 2026-11-06)") {
    CaretGhostState g;
    NoteFollowedCaret(2466, 1733, 1782, g);                            // typing in 49 px lines
    const TrackBox box{ 2400, 1700, 3000, 1800 };
    const TrackBox ghost{ 2921, 1735, 2923, 1781 };                    // 455 px right, 46 px tall
    CHECK(ClassifyCaret(ghost, box, true, kNotTerminal, g) == CaretSkip::LineEndGhost);
    CHECK(ClassifyCaret(ghost, box, false, kNotTerminal, g) == CaretSkip::LineEndGhost);
    CHECK(ClassifyCaret({ 2480, 1733, 2482, 1782 }, box, true, kNotTerminal, g) == CaretSkip::None);  // next letter
    CHECK_FALSE(g.holding);                                            // the classifier changes no state
}

TEST_CASE("caret skip: outside beats ghost, and parked beats ghost") {
    CaretGhostState g;
    NoteKnownGhost(2070, 1170, 1208, g);                               // the outside caret is also a known ghost
    CHECK(ClassifyCaret({ 2070, 1170, 2072, 1208 }, { 1520, 1022, 2372, 1108 }, true, kNotTerminal, g) == CaretSkip::OutsideElement);
    CHECK(ClassifyCaret({ 2070, 1170, 2072, 1208 }, { 1520, 1022, 2372, 1108 }, false, kNotTerminal, g) == CaretSkip::LineEndGhost);

    CaretGhostState t;
    NoteKnownGhost(985, 100, 120, t);                                  // the parked cursor is also a known ghost
    CHECK(ClassifyCaret({ 985, 100, 995, 120 }, { 0, 0, 1000, 800 }, true, kTerminal, t) == CaretSkip::ParkedTerminal);
    CHECK(ClassifyCaret({ 985, 100, 995, 120 }, { 0, 0, 1000, 800 }, true, kNotTerminal, t) == CaretSkip::LineEndGhost);
}
