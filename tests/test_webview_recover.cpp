#include "doctest.h"
#include "../src/config_ui/webview_recover.h"
#include <cstring>
using namespace wind;

TEST_CASE("a dead browser engine is recreated, a dead page reloaded, the rest left to WebView2") {
    WvRecoverBudget b;
    CHECK(DecideWvRecovery(0, b, 1000) == WvRecovery::Recreate);
    CHECK(DecideWvRecovery(1, b, 2000) == WvRecovery::Reload);
    CHECK(DecideWvRecovery(2, b, 3000) == WvRecovery::None);    // merely slow: left alone, not counted
    WvRecoverBudget c;
    CHECK(DecideWvRecovery(3, c, 1000) == WvRecovery::None);    // GPU process: WebView2 restarts it
    CHECK(DecideWvRecovery(5, c, 1000) == WvRecovery::None);
    CHECK(c.n == 0);                                             // not counted against the budget
}
TEST_CASE("an engine that dies on every start gives up after three in a minute") {
    WvRecoverBudget b;
    CHECK(DecideWvRecovery(0, b, 0) == WvRecovery::Recreate);
    CHECK(DecideWvRecovery(0, b, 1000) == WvRecovery::Recreate);
    CHECK(DecideWvRecovery(0, b, 2000) == WvRecovery::Recreate);
    CHECK(DecideWvRecovery(0, b, 3000) == WvRecovery::GiveUp);
    CHECK(DecideWvRecovery(0, b, 59999) == WvRecovery::GiveUp);
    CHECK(DecideWvRecovery(0, b, 60001) == WvRecovery::Recreate);   // the first one aged out
}
