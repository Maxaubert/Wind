// tests/test_log_queue.cpp - the non-blocking log queue (#361).
#include "doctest.h"
#include "../src/log_queue.h"
#include <thread>
#include <vector>
using namespace wind;

TEST_CASE("LogQueue pops in push order") {
    static LogQueue<int, 8> q;
    for (int i = 0; i < 5; ++i) CHECK(q.push([i](int& v) { v = i; }));
    for (int i = 0; i < 5; ++i) {
        int got = -1;
        CHECK(q.pop([&](const int& v) { got = v; }));
        CHECK(got == i);
    }
    CHECK_FALSE(q.pop([](const int&) {}));
}

TEST_CASE("LogQueue full drops instead of waiting") {
    static LogQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) CHECK(q.push([i](int& v) { v = i; }));
    bool called = false;
    CHECK_FALSE(q.push([&](int&) { called = true; }));
    CHECK_FALSE(called);                     // a full queue never touches the entry
    int got = -1;
    CHECK(q.pop([&](const int& v) { got = v; }));
    CHECK(got == 0);
    CHECK(q.push([](int& v) { v = 99; }));   // one slot free again
}

TEST_CASE("LogQueue ready() tracks the next entry across wraparound") {
    static LogQueue<int, 4> q;
    CHECK_FALSE(q.ready());
    for (int round = 0; round < 10; ++round) {
        CHECK(q.push([round](int& v) { v = round; }));
        CHECK(q.ready());
        int got = -1;
        CHECK(q.pop([&](const int& v) { got = v; }));
        CHECK(got == round);
        CHECK_FALSE(q.ready());
    }
}

TEST_CASE("LogQueue keeps every entry from concurrent producers") {
    static LogQueue<int, 1024> q;
    const int kThreads = 4, kEach = 20000;
    std::atomic<int> pushed{0}, dropped{0};
    std::atomic<bool> done{false};
    long long sum = 0; int popped = 0;
    std::thread consumer([&] {
        for (;;) {
            bool any = q.pop([&](const int& v) { sum += v; ++popped; });
            if (!any && done.load() && !q.ready()) break;
        }
    });
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t)
        producers.emplace_back([&] {
            for (int i = 1; i <= kEach; ++i) {
                if (q.push([i](int& v) { v = i; })) pushed++; else dropped++;
            }
        });
    for (auto& p : producers) p.join();
    done = true;
    consumer.join();
    CHECK(popped == pushed.load());
    CHECK(pushed.load() + dropped.load() == kThreads * kEach);
    if (dropped.load() == 0) CHECK(sum == (long long)kThreads * kEach * (kEach + 1) / 2);
}
