#include "../third_party/doctest.h"
#include "../src/tray_publish.h"
#include "../src/tray_ipc.h"
#include <memory>

using namespace wind;

static std::unique_ptr<TrayShared> Fresh() {
    auto b = std::make_unique<TrayShared>();
    InitTrayBlock(*b, 1);
    return b;
}

TEST_CASE("the taskbar, tray, alt-tab, Start and Wind's own windows are never real") {
    CHECK(ClassifyTrayForeground("Shell_TrayWnd", "explorer.exe") == TrayFgKind::Ignore);
    CHECK(ClassifyTrayForeground("Shell_SecondaryTrayWnd", "explorer.exe") == TrayFgKind::Ignore);
    CHECK(ClassifyTrayForeground("TaskListThumbnailWnd", "explorer.exe") == TrayFgKind::Ignore);
    CHECK(ClassifyTrayForeground("XamlExplorerHostIslandWindow", "explorer.exe") == TrayFgKind::Ignore);
    CHECK(ClassifyTrayForeground("TopLevelWindowForOverflowXamlIsland", "explorer.exe") == TrayFgKind::Ignore);
    CHECK(ClassifyTrayForeground("MultitaskingViewFrame", "explorer.exe") == TrayFgKind::Ignore);
    CHECK(ClassifyTrayForeground("Windows.UI.Core.CoreWindow", "searchhost.exe") == TrayFgKind::Ignore);
    // Wind's windows: the tray flyout, Settings, the core's helper, by exe or class.
    CHECK(ClassifyTrayForeground("WindTrayFlyout", "windtray.exe") == TrayFgKind::Ignore);
    CHECK(ClassifyTrayForeground("Chrome_WidgetWin_1", "windconfig.exe") == TrayFgKind::Ignore);
    CHECK(ClassifyTrayForeground("WindFocusStealer", "wind.exe") == TrayFgKind::Ignore);
    CHECK(ClassifyTrayForeground("ScreenClippingHost", "screenclippinghost.exe") == TrayFgKind::Ignore);
}

TEST_CASE("class and exe matching is case-insensitive") {
    CHECK(ClassifyTrayForeground("SHELL_TRAYWND", "explorer.exe") == TrayFgKind::Ignore);
    CHECK(ClassifyTrayForeground("Anything", "WindTray.exe") == TrayFgKind::Ignore);
}

TEST_CASE("real apps count, including File Explorer windows") {
    CHECK(ClassifyTrayForeground("CabinetWClass", "explorer.exe") == TrayFgKind::App);
    CHECK(ClassifyTrayForeground("UnrealWindow", "rdr2.exe") == TrayFgKind::App);
    CHECK(ClassifyTrayForeground("Chrome_WidgetWin_1", "chrome.exe") == TrayFgKind::App);
}

TEST_CASE("the desktop is its own kind, with or without a live-wallpaper WorkerW") {
    CHECK(ClassifyTrayForeground("Progman", "explorer.exe") == TrayFgKind::Desktop);
    CHECK(ClassifyTrayForeground("WorkerW", "wallpaper64.exe") == TrayFgKind::Desktop);
}

TEST_CASE("an unreadable exe cannot be targeted, so it is not real") {
    CHECK(ClassifyTrayForeground("SomeClass", "") == TrayFgKind::Ignore);
}

TEST_CASE("category follows the engine pick's classification") {
    CHECK(TrayCategoryFor(TrayFgKind::App, true, true, false) == (int)WindowCategory::Game);
    CHECK(TrayCategoryFor(TrayFgKind::App, true, false, false) == (int)WindowCategory::Other);   // maximized app
    CHECK(TrayCategoryFor(TrayFgKind::App, false, false, true) == (int)WindowCategory::Acrylic);
    CHECK(TrayCategoryFor(TrayFgKind::App, true, true, true) == (int)WindowCategory::Game);      // game beats Mica
    CHECK(TrayCategoryFor(TrayFgKind::Desktop, true, true, false) == (int)WindowCategory::Desktop);
}

TEST_CASE("an ignored window publishes nothing, so the last real values survive") {
    const TrayFgPublish p = DecideTrayFgPublish(TrayFgKind::Ignore, true, 3, 0);
    CHECK_FALSE(p.category);
    CHECK_FALSE(p.app);
}

TEST_CASE("an app activation bumps the count even for the same exe and category") {
    const TrayFgPublish p = DecideTrayFgPublish(TrayFgKind::App, true, 0, 0);
    CHECK(p.app);
    CHECK_FALSE(p.category);            // unchanged category is not rewritten
    CHECK(DecideTrayFgPublish(TrayFgKind::App, true, 1, 0).category);
}

TEST_CASE("a periodic refresh only ever writes a changed category") {
    CHECK_FALSE(DecideTrayFgPublish(TrayFgKind::App, false, 0, 0).app);
    CHECK_FALSE(DecideTrayFgPublish(TrayFgKind::App, false, 0, 0).category);
    const TrayFgPublish p = DecideTrayFgPublish(TrayFgKind::App, false, 0, 3);
    CHECK(p.category);
    CHECK_FALSE(p.app);
}

TEST_CASE("the desktop updates the category but never the app") {
    const TrayFgPublish p = DecideTrayFgPublish(TrayFgKind::Desktop, true, 2, 0);
    CHECK(p.category);
    CHECK_FALSE(p.app);
}

TEST_CASE("the foreground round-trips through the block") {
    auto b = Fresh();
    TrayForeground f;
    REQUIRE(ReadTrayForeground(b.get(), f));
    CHECK(f.category == -1);
    CHECK(f.exe.empty());
    CHECK(f.activations == 0u);

    PublishTrayForeground(b.get(), (int)WindowCategory::Game, true, L"rdr2.exe");
    REQUIRE(ReadTrayForeground(b.get(), f));
    CHECK(f.category == (int)WindowCategory::Game);
    CHECK(f.exe == L"rdr2.exe");
    CHECK(f.activations == 1u);

    // The desktop: category only, the exe and the count stay.
    PublishTrayForeground(b.get(), (int)WindowCategory::Desktop, false, nullptr);
    REQUIRE(ReadTrayForeground(b.get(), f));
    CHECK(f.category == (int)WindowCategory::Desktop);
    CHECK(f.exe == L"rdr2.exe");
    CHECK(f.activations == 1u);

    // Back to the same game: a new activation of the same exe.
    PublishTrayForeground(b.get(), (int)WindowCategory::Game, true, L"rdr2.exe");
    REQUIRE(ReadTrayForeground(b.get(), f));
    CHECK(f.activations == 2u);
}

TEST_CASE("an over-long exe name publishes as empty rather than truncated") {
    auto b = Fresh();
    std::wstring longName(kTrayFgExeChars + 10, L'a');
    PublishTrayForeground(b.get(), 3, true, longName.c_str());
    TrayForeground f;
    REQUIRE(ReadTrayForeground(b.get(), f));
    CHECK(f.exe.empty());
    // The longest that fits survives intact.
    std::wstring fits(kTrayFgExeChars - 1, L'b');
    PublishTrayForeground(b.get(), 3, true, fits.c_str());
    REQUIRE(ReadTrayForeground(b.get(), f));
    CHECK(f.exe == fits);
}

TEST_CASE("a write caught in progress is not read, a foreign or missing block is not live") {
    auto b = Fresh();
    PublishTrayForeground(b.get(), 0, true, L"a.exe");
    b->fgSeq.fetch_add(1);                    // odd: pretend Wind is mid-write
    TrayForeground f;
    REQUIRE(ReadTrayForeground(b.get(), f));
    CHECK(f.exe.empty());                     // gave up cleanly, no torn string
    b->fgSeq.fetch_add(1);
    REQUIRE(ReadTrayForeground(b.get(), f));
    CHECK(f.exe == L"a.exe");
    b->version.store(TrayShared::kVersion - 1);   // an older Wind's block
    CHECK_FALSE(ReadTrayForeground(b.get(), f));
    CHECK_FALSE(ReadTrayForeground(nullptr, f));
    PublishTrayForeground(nullptr, 0, true, L"x.exe");   // no-op
}

TEST_CASE("pause round-trips, bumps the command counter, and a foreign block reads as running") {
    auto b = Fresh();
    CHECK_FALSE(TrayPaused(b.get()));
    const uint32_t c0 = b->cmdSeq.load();
    SetTrayPaused(b.get(), true);
    CHECK(TrayPaused(b.get()));
    CHECK(b->cmdSeq.load() == c0 + 1);
    SetTrayPaused(b.get(), false);
    CHECK_FALSE(TrayPaused(b.get()));
    SetTrayPaused(b.get(), true);
    b->magic.store(0);
    CHECK_FALSE(TrayPaused(b.get()));     // a stale "paused" must never leave Wind dead
    CHECK_FALSE(TrayPaused(nullptr));
    SetTrayPaused(nullptr, true);         // no-op
}

TEST_CASE("stamping a block clears a stale pause and the foreground") {
    auto b = Fresh();
    SetTrayPaused(b.get(), true);
    PublishTrayForeground(b.get(), 0, true, L"x.exe");
    InitTrayBlock(*b, 2);
    CHECK_FALSE(TrayPaused(b.get()));
    TrayForeground f;
    REQUIRE(ReadTrayForeground(b.get(), f));
    CHECK(f.exe.empty());
    CHECK(f.category == -1);
}

TEST_CASE("pause blanks every zoom input") {
    ZoomInputs in;
    in.inPlain = true; in.inQz = true; in.outPlain = false; in.outQz = false; in.wheelSteps = 3;
    GateZoomInputsForPause(true, false, in);
    CHECK_FALSE(in.inPlain);
    CHECK_FALSE(in.inQz);
    CHECK_FALSE(in.outPlain);
    CHECK_FALSE(in.outQz);
    CHECK(in.wheelSteps == 0);
}

TEST_CASE("pausing while zoomed zooms out instead of freezing the view") {
    ZoomInputs in;
    in.inPlain = true; in.wheelSteps = -2;
    GateZoomInputsForPause(true, true, in);
    CHECK_FALSE(in.inPlain);
    CHECK(in.outPlain);
    CHECK(in.outQz);
    CHECK(in.wheelSteps == 0);
}

TEST_CASE("not paused leaves the zoom inputs untouched") {
    ZoomInputs in;
    in.inPlain = true; in.wheelSteps = 2;
    GateZoomInputsForPause(false, true, in);
    CHECK(in.inPlain);
    CHECK_FALSE(in.outPlain);
    CHECK(in.wheelSteps == 2);
}
