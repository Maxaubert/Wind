#include "doctest.h"
#include "../src/tx_cursor.h"

using namespace wind;

TEST_CASE("tx cursor mode parses, and anything unknown is off") {
    CHECK(ParseTxCursorMode(0) == TxCursorMode::Off);
    CHECK(ParseTxCursorMode(1) == TxCursorMode::Always);
    CHECK(ParseTxCursorMode(2) == TxCursorMode::OnChange);
    CHECK(ParseTxCursorMode(-1) == TxCursorMode::Off);
    CHECK(ParseTxCursorMode(99) == TxCursorMode::Off);
}

TEST_CASE("Off never publishes: the shipped behaviour is the control arm") {
    CHECK_FALSE(ShouldPublishCursorTransform(TxCursorMode::Off, 10, 20, kNoPublishedOffset, kNoPublishedOffset));
    CHECK_FALSE(ShouldPublishCursorTransform(TxCursorMode::Off, 10, 20, 999, 999));
}

TEST_CASE("Always publishes every tick, even when nothing moved") {
    CHECK(ShouldPublishCursorTransform(TxCursorMode::Always, 10, 20, 10, 20));
    CHECK(ShouldPublishCursorTransform(TxCursorMode::Always, 0, 0, kNoPublishedOffset, kNoPublishedOffset));
}

TEST_CASE("OnChange publishes only when the whole-pixel offset actually moves") {
    CHECK_FALSE(ShouldPublishCursorTransform(TxCursorMode::OnChange, 10, 20, 10, 20));
    CHECK(ShouldPublishCursorTransform(TxCursorMode::OnChange, 11, 20, 10, 20));
    CHECK(ShouldPublishCursorTransform(TxCursorMode::OnChange, 10, 21, 10, 20));
}

// The regression this guards: a session that starts without publishing would leave DWM holding
// the previous session's cursor transform, so the very first write must always go public.
TEST_CASE("OnChange always publishes the first write of a session") {
    CHECK(ShouldPublishCursorTransform(TxCursorMode::OnChange, 0, 0, kNoPublishedOffset, kNoPublishedOffset));
    CHECK(ShouldPublishCursorTransform(TxCursorMode::OnChange, 10, 20, kNoPublishedOffset, kNoPublishedOffset));
}
