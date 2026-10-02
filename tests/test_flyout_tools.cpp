// The tray tools (issue #315): the main-engine dropdown (values, labels, restart rule, view), and the
// chip rows (centring, the wide engine chip, wrapping, hit testing, arrow-key neighbours). All pure;
// engine_dropdown.cpp / flyout_window.cpp wire them to Win32.
#include "../third_party/doctest.h"
#include "../src/tray_app/flyout_model.h"
#include "../src/tray_app/flyout_icons.h"
#include "../src/config.h"
#include <algorithm>
#include <vector>
using namespace wind;
using namespace wind::Flyout;

// ---------------------------------------------------------------- engine dropdown

TEST_CASE("engine: the options are the Settings main-engine row, same order and labels") {
    CHECK(kEngineCount == 4);
    CHECK(std::string(kEngineKey) == "model");
    CHECK(std::string(EngineValue(0)) == "hybrid");
    CHECK(std::string(EngineValue(1)) == "render");
    CHECK(std::string(EngineValue(2)) == "transform");
    CHECK(std::string(EngineValue(3)) == "magnify");
    CHECK(std::wstring(EngineLabel(0)) == L"Auto");
    CHECK(std::wstring(EngineLabel(1)) == L"Render");
    CHECK(std::wstring(EngineLabel(2)) == L"Transform");
    CHECK(std::wstring(EngineLabel(3)) == L"System");
}

TEST_CASE("engine: every option value is one the core accepts as is") {
    for (int i = 0; i < kEngineCount; ++i) {
        Config c = ParseConfig(std::string("model=") + EngineValue(i) + "\n");
        CHECK(c.model == EngineValue(i));
    }
}

TEST_CASE("engine: a model value maps to its option, unknown reads as Auto like the core") {
    CHECK(EngineIndex("hybrid") == 0);
    CHECK(EngineIndex("render") == 1);
    CHECK(EngineIndex("transform") == 2);
    CHECK(EngineIndex("magnify") == 3);
    CHECK(EngineIndex(" transform ") == 2);
    CHECK(EngineIndex("") == 0);
    CHECK(EngineIndex("junk") == 0);
}

TEST_CASE("engine: only a different pick changes anything (and so restarts Wind)") {
    CHECK_FALSE(EnginePickChanges(2, 2));
    CHECK(EnginePickChanges(0, 2));
    CHECK(EnginePickChanges(3, 0));
    CHECK_FALSE(EnginePickChanges(0, -1));
    CHECK_FALSE(EnginePickChanges(0, kEngineCount));
}

static View ViewWith(const char* toggles, const IniValues& extra = {}) {
    IniValues v = extra;
    v["trayToggles"] = toggles;
    return BuildView(v, ParseTrayLayout(v), TrayStatus(), nullptr, 0, L"Default", true);
}

TEST_CASE("view: the engine chip shows the main engine as text, is wide and never ON") {
    const View v = ViewWith("engine", { { "model", "transform" } });
    REQUIRE(v.toggles.size() == 1);
    const ToggleView& t = v.toggles[0];
    CHECK(t.kind == ChipKind::Engine);
    CHECK(t.value == L"Transform");
    CHECK_FALSE(t.on);
    CHECK_FALSE(t.open);
    CHECK(ChipSlots(t.kind) == 2);
    CHECK(ToggleSlots(v) == std::vector<int>{ 2 });
}

TEST_CASE("view: a missing or unknown model reads Auto, and the chip is never disabled by it") {
    CHECK(ViewWith("engine").toggles[0].value == L"Auto");
    CHECK(ViewWith("engine", { { "model", "junk" } }).toggles[0].value == L"Auto");
    CHECK(ViewWith("engine", { { "model", "magnify" } }).toggles[0].value == L"System");
    CHECK(ViewWith("engine", { { "model", "render" } }).toggles[0].value == L"Render");
}

TEST_CASE("view: plain toggles stay one slot with no value text") {
    const View v = ViewWith("trackCaret,engine,keepEdges");
    REQUIRE(v.toggles.size() == 3);
    CHECK(ToggleSlots(v) == std::vector<int>{ 1, 2, 1 });
    CHECK(v.toggles[0].value.empty());
}

TEST_CASE("view: the engine item has a name and icons for the chip and its chevrons") {
    const ToggleSpec* s = FindToggleSpec("engine");
    REQUIRE(s != nullptr);
    CHECK(IconPath(s->icon) != nullptr);
    CHECK(IconPath("chevdown") != nullptr);
    CHECK(IconPath("chevup") != nullptr);
    CHECK(ChipKindOf("engine") == ChipKind::Engine);
    CHECK(ChipKindOf("trackCaret") == ChipKind::Plain);
}

TEST_CASE("view: keys of the removed tools (fixLock, fixPass, pause) are not items any more") {
    CHECK(FindToggleSpec("fixLock") == nullptr);
    CHECK(FindToggleSpec("fixPass") == nullptr);
    CHECK(FindToggleSpec("pause") == nullptr);
    CHECK(IconPath("lock") == nullptr);
    CHECK(IconPath("pass") == nullptr);
    CHECK(IconPath("pause") == nullptr);
    // A user's ini that still lists them shows only the known chips.
    const View v = ViewWith("fixLock,trackCaret,pause,fixPass");
    REQUIRE(v.toggles.size() == 1);
    CHECK(v.toggles[0].key == "trackCaret");
}

// ---------------------------------------------------------------- chip rows: centring

static int Left(const Geometry& g) { return kBorder; }
static int Right(const Geometry& g) { return g.width - kBorder; }
// Space left of the first chip and right of the last chip of the row that contains chip `first`.
static void RowMargins(const Geometry& g, size_t first, size_t last, int* l, int* r) {
    *l = g.chip[first].l - Left(g);
    *r = Right(g) - g.chip[last].r;
}

TEST_CASE("centring: 3 chips sit centred, equal space left and right") {
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>{ 1, 1, 1 }, 40);
    REQUIRE(g.chip.size() == 3);
    int l = 0, r = 0;
    RowMargins(g, 0, 2, &l, &r);
    CHECK(l == r);
    CHECK(l == 67);
    CHECK(g.chip[1].l - g.chip[0].r == kChipGap);
    for (const IRect& c : g.chip) { CHECK(c.w() == kChipW); CHECK(c.h() == kChipH); }
}

TEST_CASE("centring: 4 chips fill the row, still centred") {
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>{ 1, 1, 1, 1 }, 40);
    int l = 0, r = 0;
    RowMargins(g, 0, 3, &l, &r);
    CHECK(l == r);
    CHECK(l == 38);
    CHECK(g.chip[3].t == g.chip[0].t);          // one row
}

TEST_CASE("centring: 2 and 1 chips are centred too") {
    for (int n : { 1, 2 }) {
        const Geometry g = ComputeGeometry(false, 0, std::vector<int>((size_t)n, 1), 40);
        int l = 0, r = 0;
        RowMargins(g, 0, (size_t)n - 1, &l, &r);
        CHECK(l == r);
    }
}

TEST_CASE("centring: 5 chips wrap to 4 + 1, and the last partial row is centred on its own") {
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>(5, 1), 40);
    REQUIRE(g.chip.size() == 5);
    int l = 0, r = 0;
    RowMargins(g, 0, 3, &l, &r);
    CHECK(l == r);
    CHECK(g.chip[4].t - g.chip[0].t == kChipRowH);
    RowMargins(g, 4, 4, &l, &r);
    CHECK(l == r);                                // the lone chip: equal space both sides
    CHECK(g.chip[4].l == 1 + (298 - kChipW) / 2);
    for (const IRect& c : g.chip) CHECK(c.w() == kChipW);
}

TEST_CASE("centring: 7 chips are 4 + 3, both rows centred, one more row of height") {
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>(7, 1), 40);
    int l = 0, r = 0;
    RowMargins(g, 4, 6, &l, &r);
    CHECK(l == r);
    const Geometry one = ComputeGeometry(false, 0, std::vector<int>(4, 1), 40);
    CHECK(g.height - one.height == kChipRowH);
}

TEST_CASE("centring: the wide engine chip spans two slots, 2 * chipW + gap") {
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>{ 1, 1, 1, 2 }, 40);
    REQUIRE(g.chip.size() == 4);
    CHECK(g.chip[3].w() == 2 * kChipW + kChipGap);
    CHECK(g.chip[3].h() == kChipH);
    // 1+1+1+2 = 5 slots: the engine chip does not fit the first row, so it wraps and is centred alone.
    CHECK(g.chip[3].t - g.chip[0].t == kChipRowH);
    int l = 0, r = 0;
    RowMargins(g, 3, 3, &l, &r);
    CHECK(l == r);
    RowMargins(g, 0, 2, &l, &r);
    CHECK(l == r);
}

TEST_CASE("centring: the engine chip beside two chips fills the row exactly (2 + 1 + 1 slots)") {
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>{ 1, 1, 2 }, 40);
    CHECK(g.chip[2].t == g.chip[0].t);
    int l = 0, r = 0;
    RowMargins(g, 0, 2, &l, &r);
    CHECK(l == r);
    CHECK(l == 38);
    CHECK(g.chip[2].l - g.chip[1].r == kChipGap);
}

TEST_CASE("centring: the engine chip alone is centred") {
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>{ 2 }, 40);
    int l = 0, r = 0;
    RowMargins(g, 0, 0, &l, &r);
    CHECK(l == r);
    CHECK(g.chip[0].w() == 106);
}

TEST_CASE("centring: every row's margins match for any mix of chips, at every count") {
    const std::vector<std::vector<int>> mixes = {
        { 2, 1, 1 }, { 1, 2, 1 }, { 1, 1, 2 }, { 2, 2 }, { 2, 2, 2 }, { 1, 2, 2, 1 }, { 1, 1, 1, 1, 2, 1 } };
    for (const auto& m : mixes) {
        const Geometry g = ComputeGeometry(false, 0, m, 40);
        size_t i = 0;
        while (i < g.chip.size()) {
            size_t j = i;
            while (j + 1 < g.chip.size() && g.chip[j + 1].t == g.chip[i].t) ++j;
            int l = 0, r = 0;
            RowMargins(g, i, j, &l, &r);
            CHECK(l == r);
            CHECK(l >= 0);
            i = j + 1;
        }
    }
}

TEST_CASE("centring: a row never holds more than four slots") {
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>{ 1, 1, 1, 1, 1, 1, 1, 1, 1 }, 40);
    for (size_t i = 0; i < g.chip.size(); ++i) {
        int inRow = 0;
        for (const IRect& c : g.chip) if (c.t == g.chip[i].t) ++inRow;
        CHECK(inRow <= kChipsPerRow);
    }
}

TEST_CASE("layout: no chips, no chip rows; the window with 3 chips keeps its height") {
    CHECK(ComputeGeometry(true, 2, std::vector<int>{ 1, 1, 1 }, 40).height == 283);
    CHECK(ComputeGeometry(false, 0, std::vector<int>{}, 40).chip.empty());
}

// ---------------------------------------------------------------- chip rows: hit testing

TEST_CASE("hit test: the whole wide chip is one hit, the gap and the margins are none") {
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>{ 1, 1, 2 }, 40);
    const IRect& e = g.chip[2];
    const int my = (e.t + e.b) / 2;
    CHECK(HitTest(g, e.l + 1, my) == Hit{ HitKind::Chip, 2 });                 // the icon end
    CHECK(HitTest(g, (e.l + e.r) / 2, my) == Hit{ HitKind::Chip, 2 });         // the text
    CHECK(HitTest(g, e.r - 1, my) == Hit{ HitKind::Chip, 2 });                 // the chevron end
    CHECK(HitTest(g, e.r, my).kind == HitKind::None);
    CHECK(HitTest(g, e.l - 1, my) == Hit{ HitKind::None, -1 });                // the gap before it
    CHECK(HitTest(g, g.chip[0].l - 3, my).kind == HitKind::None);              // the left margin
    CHECK(HitTest(g, g.chip[0].l, g.chip[0].t) == Hit{ HitKind::Chip, 0 });
    CHECK(HitTest(g, g.chip[1].l, g.chip[1].t) == Hit{ HitKind::Chip, 1 });
}

TEST_CASE("hit test: chips of a centred partial row are hit where they are drawn") {
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>(5, 1), 40);
    const IRect& c = g.chip[4];
    CHECK(HitTest(g, (c.l + c.r) / 2, (c.t + c.b) / 2) == Hit{ HitKind::Chip, 4 });
    CHECK(HitTest(g, g.chip[0].l, c.t + 2).kind == HitKind::None);             // where a left-aligned chip used to be
}

// ---------------------------------------------------------------- chip rows: keyboard

TEST_CASE("keyboard: Left and Right walk the chips in order and stop at the ends") {
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>{ 1, 1, 2, 1 }, 40);
    CHECK(ChipNeighbor(g, 0, +1, 0) == 1);
    CHECK(ChipNeighbor(g, 3, +1, 0) == 3);
    CHECK(ChipNeighbor(g, 0, -1, 0) == 0);
    CHECK(ChipNeighbor(g, 2, -1, 0) == 1);
}

TEST_CASE("keyboard: Up and Down move to the nearest chip of the row above or below") {
    // 1,1,2 | 1  : row one has three chips (the third is the wide engine chip), row two has one
    const Geometry g = ComputeGeometry(false, 0, std::vector<int>{ 1, 1, 2, 1 }, 40);
    REQUIRE(g.chip[3].t > g.chip[0].t);
    CHECK(ChipNeighbor(g, 0, 0, +1) == 3);
    CHECK(ChipNeighbor(g, 2, 0, +1) == 3);
    CHECK(ChipNeighbor(g, 3, 0, -1) == 1);   // the row-two chip is centred, nearest above is the second chip or the engine
    CHECK(ChipNeighbor(g, 3, 0, +1) == 3);   // no row below: stays
    CHECK(ChipNeighbor(g, 0, 0, -1) == 0);   // no row above: stays
}
