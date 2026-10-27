// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#pragma once
// Wind needs UIAccess (issue #460). Without it the transform engine has hover dead zones, and
// neither engine can magnify over the shell or elevated windows, so a Wind without it refuses to
// start and says how to fix it, instead of running degraded. That happens with a plain build.bat
// build and with an install where Setup's local signing failed. (A uiAccess build that Windows does
// not trust never starts at all, so it never gets here.)
//
// Developers can still run a plain build: WIND_ALLOW_NO_UIACCESS=1, or WIND_SELFTEST=1 (the
// self-test recipe runs a scratch plain Wind.exe). Pure, no <windows.h>.
namespace wind {

inline bool MustRefuseWithoutUiAccess(bool tokenUiAccess, bool allowEnv, bool selftestEnv) {
    return !tokenUiAccess && !allowEnv && !selftestEnv;
}

// The refusal message (UI copy, no em-dashes).
inline const wchar_t* UiAccessRefusalTitle() { return L"Wind can't start"; }
inline const wchar_t* UiAccessRefusalText() {
    return L"Wind needs UI Access to magnify the screen properly, and Windows gives it only to a "
           L"Wind that Setup has installed and signed on this PC.\n\n"
           L"Run the Wind installer again (it asks for admin rights), then start Wind from the "
           L"Start menu.";
}
}  // namespace wind
