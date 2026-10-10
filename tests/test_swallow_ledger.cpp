// tests/test_swallow_ledger.cpp - the hook swallow records (swallow_ledger.h).
#include "doctest.h"
#include "../src/swallow_ledger.h"
#include <thread>
using namespace wind;

TEST_CASE("an UP is swallowed only when its DOWN was") {
    SwallowLedger<8> l;
    CHECK_FALSE(l.consumeUp(3));          // never swallowed a DOWN: the UP passes through
    l.markDown(3);
    CHECK(l.isSet(3));                    // auto-repeat of the held key
    CHECK(l.consumeUp(3));
    CHECK_FALSE(l.isSet(3));
    CHECK_FALSE(l.consumeUp(3));          // consumed: a later unrelated UP passes through
}

TEST_CASE("slots are independent (a chord never strands an UP)") {
    SwallowLedger<8> l;
    l.markDown(1); l.markDown(2);
    CHECK(l.consumeUp(2));
    CHECK(l.isSet(1));
    CHECK(l.consumeUp(1));
}

TEST_CASE("clear and clearAll drop the records; out-of-range ids are inert") {
    SwallowLedger<4> l;
    l.markDown(0); l.markDown(1); l.markDown(2);
    l.clear(1);
    CHECK_FALSE(l.isSet(1));
    CHECK(l.isSet(0));
    l.clearAll();
    CHECK_FALSE(l.isSet(0));
    CHECK_FALSE(l.isSet(2));
    l.markDown(4); l.markDown(1000);      // ignored, no overrun
    CHECK_FALSE(l.isSet(4));
    CHECK_FALSE(l.consumeUp(1000));
}

TEST_CASE("a DOWN marked on one thread is consumed at most once per mark on another") {
    static SwallowLedger<2> l;
    std::atomic<int> consumed{0};
    std::thread t1([&] { for (int i = 0; i < 20000; ++i) l.markDown(1); });
    std::thread t2([&] { for (int i = 0; i < 20000; ++i) if (l.consumeUp(1)) consumed++; });
    t1.join(); t2.join();
    if (l.consumeUp(1)) consumed++;
    CHECK(consumed.load() >= 1);
    CHECK(consumed.load() <= 20000);
    CHECK_FALSE(l.consumeUp(1));
}
