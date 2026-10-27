// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The process token's UIAccess bit (Win32). MUST be included AFTER <windows.h>. The pure start-up
// rule that uses it is uiaccess_gate.h; this is the one place Wind reads the bit (issues #185, #460).
//
// Two probe shapes are BANNED for this question (self-review, rig-measured): a DISABLED
// MagSetInputTransform succeeds WITHOUT UIAccess (false positive), and any Mag acquire/release at
// startup runs MagHost::shutdown's identity transform WRITE, which violates the no-warm-up rule and
// resets a running native Magnifier's zoom. Reading the token makes ZERO Magnification calls.
#pragma once
namespace wind {

// True when this process runs with UIAccess. A token that cannot be opened or read counts as none.
inline bool ProcessHasUiAccess() {
    DWORD uiAccess = 0, len = 0;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (!GetTokenInformation(tok, TokenUIAccess, &uiAccess, sizeof(uiAccess), &len)) uiAccess = 0;
        CloseHandle(tok);
    }
    return uiAccess != 0;
}

}  // namespace wind
