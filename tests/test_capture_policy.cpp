// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "doctest.h"
#include "capture_policy.h"
using namespace wind;

TEST_CASE("duplication retry: immediate first try, then 250 ms doubling to a 2 s cap") {
    DuplRetry r;
    CHECK(r.due(0));
    r.fail(1000);
    CHECK(r.delayMs == 250);
    CHECK_FALSE(r.due(1249));
    CHECK(r.due(1250));
    r.fail(1250); CHECK(r.delayMs == 500);
    r.fail(1750); CHECK(r.delayMs == 1000);
    r.fail(2750); CHECK(r.delayMs == 2000);
    r.fail(4750); CHECK(r.delayMs == 2000);   // capped
    CHECK(r.failures == 5);
    r.reset();
    CHECK(r.due(0));
    CHECK(r.failures == 0);
}

TEST_CASE("duplication retry logs only power-of-two failures") {
    DuplRetry r;
    CHECK_FALSE(r.logThisFailure());
    int logged = 0;
    for (int i = 0; i < 100; ++i) { r.fail((unsigned long long)i * 5000); if (r.logThisFailure()) ++logged; }
    CHECK(logged == 7);   // 1 2 4 8 16 32 64
}

TEST_CASE("crop debt: a crop is paid by the next uncropped frame") {
    CropDebt d;
    CHECK_FALSE(d.fullCopyNow(false));
    d.noteCropped(100);
    CHECK_FALSE(d.fullCopyNow(true));   // still a full repaint: keep cropping
    CHECK(d.fullCopyNow(false));        // the screen settled into small changes: copy whole
    d.paid();
    CHECK_FALSE(d.fullCopyNow(false));
}

TEST_CASE("crop debt: a quiet screen asks for one fresh grab per quiet window") {
    CropDebt d;
    CHECK_FALSE(d.refreshDue(10000, 250));   // nothing owed
    d.noteCropped(1000);
    CHECK_FALSE(d.refreshDue(1249, 250));
    CHECK(d.refreshDue(1250, 250));
    d.refreshAsked(1250);
    CHECK_FALSE(d.refreshDue(1400, 250));    // not re-asked every tick
    CHECK(d.refreshDue(1500, 250));          // the grab did not land: ask again later
    d.paid();
    CHECK_FALSE(d.refreshDue(5000, 250));
}

TEST_CASE("fresh drain: 100 ms for the first frame, then 12 ms of draining") {
    CHECK(FreshDrainContinue(false, 0.0, 0.0));
    CHECK(FreshDrainContinue(false, 99.0, 0.0));
    CHECK_FALSE(FreshDrainContinue(false, 100.0, 0.0));
    CHECK(FreshDrainContinue(true, 5.0, 0.0));
    CHECK(FreshDrainContinue(true, 50.0, 11.9));   // the first-frame budget no longer applies
    CHECK_FALSE(FreshDrainContinue(true, 20.0, 12.0));   // fast panel: frames keep coming, stop anyway
}
