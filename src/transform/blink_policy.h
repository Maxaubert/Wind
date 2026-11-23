// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#pragma once
// Pure capture blink rules (no <windows.h>), used by the Wind.CaptureBlink.v1 handler in
// src/core/main.cpp and by BlinkCapture (src/transform/blink_capture.cpp). A window message carries
// no sender identity and UIPI is opened for this one message, so anything on the desktop can ask
// for a blink and name any pid (lParam). These rules decide whom Wind answers and into what section
// it writes. Protocol: docs/architecture/05-transform-engine.md, "Capture blink".
// Tests: tests/test_blink_policy.cpp.
#include <cstdint>
#include <string_view>

namespace wind {

// Mandatory integrity RIDs (winnt.h SECURITY_MANDATORY_*_RID), defined here so this stays pure.
constexpr uint32_t kBlinkLowRid = 0x1000;
constexpr uint32_t kBlinkMediumRid = 0x2000;
// The shortest gap between two blink attempts. Caps the flicker and the tick-thread stall a flood
// of requests could cause; Glint asks at most once per snip.
constexpr uint64_t kBlinkMinGapMs = 250;

// The file name part of a full image path (after the last '\' or '/').
inline std::wstring_view BlinkExeBase(std::wstring_view path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring_view::npos ? path : path.substr(cut + 1);
}

// ASCII case-insensitive equality (exe names; other characters compare exactly).
inline bool BlinkEqualsNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = static_cast<wchar_t>(y - L'A' + L'a');
        if (x != y) return false;
    }
    return true;
}

// Sender rule: the claimed pid runs at Medium integrity or above, outside an AppContainer, in
// Wind's session, from an image named glint.exe. The pid is only a claim: a Low IL or sandboxed
// caller can name a running Glint's pid. What stops it taking the frame is the section rule below,
// which requires Glint's Medium no-read-up label, so a Low IL process cannot open the section for
// reading. A Medium same-user process is not a boundary: it could read Glint's memory or the
// screen anyway.
inline bool BlinkSenderOk(uint32_t ilRid, bool appContainer, bool sameSession, std::wstring_view exeBase) {
    return ilRid >= kBlinkMediumRid && !appContainer && sameSession && BlinkEqualsNoCase(exeBase, L"glint.exe");
}

// Section label rule: the section carries a mandatory label of Medium or above WITH the no-read-up
// policy, as Glint 0.3.3 creates it (SDDL S:(ML;;NWNR;;;ME)). Without it a Low IL process could read
// the frame Wind writes, so Wind writes nothing (Glint then keeps its normal capture). A section with
// no label (Glint 0.3.2 and older, an implicit Medium no-write-up label) or a Low label is refused.
constexpr uint32_t kBlinkNoReadUp = 0x2;   // SYSTEM_MANDATORY_LABEL_NO_READ_UP
inline bool BlinkSectionLabelOk(bool hasLabel, uint32_t labelRid, uint32_t labelPolicy) {
    return hasLabel && labelRid >= kBlinkMediumRid && (labelPolicy & kBlinkNoReadUp) != 0;
}

// Section view rule: the mapped view is committed (not a SEC_RESERVE section whose pages fault),
// is a mapped section (not an image) and its region holds the whole frame.
inline bool BlinkSectionOk(uint64_t regionSize, bool committed, bool mapped, uint64_t need) {
    return committed && mapped && regionSize >= need;
}

// Rate rule: at least kBlinkMinGapMs since the last attempt; lastMs == 0 means none yet.
inline bool BlinkRateOk(uint64_t nowMs, uint64_t lastMs) {
    return lastMs == 0 || (nowMs >= lastMs && nowMs - lastMs >= kBlinkMinGapMs);
}

}  // namespace wind
