// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "doctest.h"
#include "blink_policy.h"
using namespace wind;

TEST_CASE("blink sender: Glint at Medium in this session is answered") {
    CHECK(BlinkSenderOk(kBlinkMediumRid, false, true, L"glint.exe"));
    CHECK(BlinkSenderOk(kBlinkMediumRid, false, true, L"GLINT.EXE"));
    CHECK(BlinkSenderOk(kBlinkMediumRid, false, true, L"Glint.Exe"));
    CHECK(BlinkSenderOk(0x2010, false, true, L"glint.exe"));   // Medium Plus (UIAccess)
    CHECK(BlinkSenderOk(0x3000, false, true, L"glint.exe"));   // High (elevated)
}

TEST_CASE("blink sender: low integrity, sandboxed, other session or other exe is refused") {
    CHECK_FALSE(BlinkSenderOk(kBlinkLowRid, false, true, L"glint.exe"));
    CHECK_FALSE(BlinkSenderOk(0x0000, false, true, L"glint.exe"));   // untrusted
    CHECK_FALSE(BlinkSenderOk(kBlinkMediumRid - 1, false, true, L"glint.exe"));
    CHECK_FALSE(BlinkSenderOk(kBlinkMediumRid, true, true, L"glint.exe"));
    CHECK_FALSE(BlinkSenderOk(kBlinkMediumRid, false, false, L"glint.exe"));
    CHECK_FALSE(BlinkSenderOk(kBlinkMediumRid, false, true, L"glint-editor.exe"));
    CHECK_FALSE(BlinkSenderOk(kBlinkMediumRid, false, true, L"glint.exe.exe"));
    CHECK_FALSE(BlinkSenderOk(kBlinkMediumRid, false, true, L"xglint.exe"));
    CHECK_FALSE(BlinkSenderOk(kBlinkMediumRid, false, true, L""));
}

TEST_CASE("blink exe base: the file name after the last separator") {
    CHECK(BlinkExeBase(L"C:\\Users\\me\\AppData\\Local\\Programs\\Glint\\glint.exe") == L"glint.exe");
    CHECK(BlinkExeBase(L"C:/tools/Glint.EXE") == L"Glint.EXE");
    CHECK(BlinkExeBase(L"glint.exe") == L"glint.exe");
    CHECK(BlinkExeBase(L"C:\\glint.exe\\evil.exe") == L"evil.exe");
    CHECK(BlinkExeBase(L"C:\\dir\\") == L"");
    CHECK(BlinkSenderOk(kBlinkMediumRid, false, true,
                        BlinkExeBase(L"\\Device\\HarddiskVolume3\\Programs\\Glint\\GLINT.exe")));
}

TEST_CASE("blink section label: none means Medium, Low is refused") {
    CHECK(BlinkSectionLabelOk(false, 0));
    CHECK(BlinkSectionLabelOk(true, kBlinkMediumRid));
    CHECK(BlinkSectionLabelOk(true, 0x3000));
    CHECK_FALSE(BlinkSectionLabelOk(true, kBlinkLowRid));
    CHECK_FALSE(BlinkSectionLabelOk(true, 0));
}

TEST_CASE("blink section: committed, mapped and large enough") {
    CHECK(BlinkSectionOk(4096, true, true, 4096));
    CHECK(BlinkSectionOk(1ull << 30, true, true, 32 + 3840ull * 2160 * 8));
    CHECK_FALSE(BlinkSectionOk(4095, true, true, 4096));
    CHECK_FALSE(BlinkSectionOk(1ull << 30, false, true, 4096));   // SEC_RESERVE, nothing committed
    CHECK_FALSE(BlinkSectionOk(1ull << 30, true, false, 4096));   // not a mapped section
    CHECK_FALSE(BlinkSectionOk(0, false, false, 0));
}

TEST_CASE("blink rate: one attempt per 250 ms") {
    CHECK(BlinkRateOk(5, 0));          // nothing yet
    CHECK(BlinkRateOk(1000000, 0));
    CHECK_FALSE(BlinkRateOk(1000, 1000));
    CHECK_FALSE(BlinkRateOk(1249, 1000));
    CHECK(BlinkRateOk(1250, 1000));
    CHECK(BlinkRateOk(9000, 1000));
    CHECK_FALSE(BlinkRateOk(999, 1000));   // a clock that went back never opens the gate early
}
