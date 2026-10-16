#include "doctest.h"
#include "java_bridge_util.h"
using namespace wind;

TEST_CASE("bridge DLL candidates: JetBrains bundled runtime comes right after the exe's own folder") {
    auto c = JavaBridgeDllCandidates(L"C:\\Program Files\\JetBrains\\IntelliJ IDEA 2026.2.1\\bin");
    REQUIRE(c.size() >= 2);
    CHECK(c[0] == L"C:\\Program Files\\JetBrains\\IntelliJ IDEA 2026.2.1\\bin\\windowsaccessbridge-64.dll");
    CHECK(c[1] == L"C:\\Program Files\\JetBrains\\IntelliJ IDEA 2026.2.1\\jbr\\bin\\windowsaccessbridge-64.dll");
    auto j = JavaBridgeDllCandidates(L"C:\\jdk\\bin\\");   // java.exe: beside itself
    CHECK(j[0] == L"C:\\jdk\\bin\\windowsaccessbridge-64.dll");
}

TEST_CASE("enable text: missing file or key appends the bridge") {
    bool ch = false;
    CHECK(EnableAccessBridgeText("", ch) == "assistive_technologies=com.sun.java.accessibility.AccessBridge\n");
    CHECK(ch);
    CHECK(EnableAccessBridgeText("screen_magnifier_present=true", ch) ==
          "screen_magnifier_present=true\nassistive_technologies=com.sun.java.accessibility.AccessBridge\n");
    CHECK(ch);
}
TEST_CASE("enable text: already enabled is left byte-for-byte alone") {
    bool ch = true;
    const std::string s = "assistive_technologies=com.sun.java.accessibility.AccessBridge\nscreen_magnifier_present=true\n";
    CHECK(EnableAccessBridgeText(s, ch) == s);
    CHECK_FALSE(ch);
}
TEST_CASE("enable text: another assistive technology is kept and the bridge added to it") {
    bool ch = false;
    CHECK(EnableAccessBridgeText("assistive_technologies=org.other.AT\r\nx=1\r\n", ch) ==
          "assistive_technologies=org.other.AT,com.sun.java.accessibility.AccessBridge\nx=1\r\n");
    CHECK(ch);
    CHECK(EnableAccessBridgeText("assistive_technologies=\n", ch) ==
          "assistive_technologies=com.sun.java.accessibility.AccessBridge\n");
}
TEST_CASE("Java window classes") {
    CHECK(IsJavaWindowClass(L"SunAwtFrame"));
    CHECK(IsJavaWindowClass(L"SunAwtDialog"));
    CHECK_FALSE(IsJavaWindowClass(L"MozillaWindowClass"));
}

TEST_CASE("Java user space converts to device px by the monitor scale, rounding half away from zero") {
    CHECK(JavaUserToPx(59, 2.25) == 133);       // 132.75
    CHECK(JavaUserToPx(100, 1.0) == 100);
    CHECK(JavaUserToPx(-10, 1.5) == -15);
    CHECK(JavaUserToPx(0, 2.25) == 0);
}
TEST_CASE("the character-bounds fallbacks land in the same device px as the caret location") {
    // A caret at user (59, 20) 1 wide, 16 high on a 225% monitor.
    const JavaRectPx caret = JavaSpanRectPx(59, 20, 1, 16, 2.25);
    CHECK(caret.left == 133); CHECK(caret.top == 45);
    CHECK(caret.right == caret.left + 2);       // never narrower than 2 px
    CHECK(caret.bottom == 81);                  // 36 * 2.25
    // Unscaled (the old fallback bug) would have put the same caret at x=59.
    const JavaRectPx unscaled = JavaSpanRectPx(59, 20, 1, 16, 1.0);
    CHECK(unscaled.left == 59);
    const JavaRectPx wide = JavaSpanRectPx(10, 0, 8, 10, 1.5);
    CHECK(wide.right - wide.left == 12);        // a real character keeps its scaled width
}
TEST_CASE("the end-of-text fallback is a 2 px sliver at the scaled right edge of the previous character") {
    const JavaRectPx r = JavaAfterRectPx(100, 20, 8, 16, 2.0);
    CHECK(r.left == 216); CHECK(r.right == 218);
    CHECK(r.top == 40); CHECK(r.bottom == 72);
}
TEST_CASE("bridge retries back off: first delay, doubling, capped, never polling at a fixed rate") {
    CHECK(BackoffMs(0, 250, 4000) == 250);
    CHECK(BackoffMs(250, 250, 4000) == 500);
    CHECK(BackoffMs(2000, 250, 4000) == 4000);
    CHECK(BackoffMs(4000, 250, 4000) == 4000);  // capped
    CHECK(BackoffMs(0, 2000, 60000) == 2000);   // the per-process probe uses its own curve
    CHECK(BackoffMs(40000, 2000, 60000) == 60000);
}
TEST_CASE("a failed bridge folder is retried on a 5 s to 5 min curve, never written off (review 2026-10-16 #31)") {
    unsigned long long b = 0;
    b = BackoffMs(b, 5000, 300000); CHECK(b == 5000);
    b = BackoffMs(b, 5000, 300000); CHECK(b == 10000);
    for (int i = 0; i < 20; ++i) b = BackoffMs(b, 5000, 300000);
    CHECK(b == 300000);
}
TEST_CASE("a caret on a scaled monitor away from (0,0): anchored at the monitor origin (review 2026-10-16 #16)") {
    // Primary 3840 px at 100%; IntelliJ on a 150% monitor at device x=3840. A caret at device
    // x=4000 is user x = 3840 + 160 / 1.5 = 3946.67, reported as 3947.
    CHECK(JavaUserToPx(3947, 1.5, 3840) == 4001);
    CHECK(JavaUserToPx(3947, 1.5) == 5921);            // the old mapping: ~1920 px too far right
    CHECK(JavaUserToPx(59, 2.25, 0) == 133);           // a monitor at (0,0): both agree
    CHECK(JavaUserToPx(-1000, 1.5, -1920) == -540);    // a monitor left of the primary
    const JavaRectPx a = JavaSpanRectPx(3947, 120, 1, 16, 1.5, 3840, 0);
    CHECK(a.left == 4001); CHECK(a.top == 180); CHECK(a.bottom == 204);
    CHECK(a.right == a.left + 2);
    const JavaRectPx after = JavaAfterRectPx(3940, 120, 4, 16, 1.5, 3840, 0);
    CHECK(after.left == 3996); CHECK(after.right == 3998);
}
TEST_CASE("the caret mapping whose centre is inside the Java window wins; anchored on a tie") {
    const JavaRectPx anchored{ 4001, 180, 4003, 204 }, legacy{ 5921, 180, 5923, 204 };
    // The window on the secondary (3840..7680): the anchored mapping is inside it.
    CHECK(JavaPickCaretPx(anchored, legacy, 3840, 0, 7680, 2160).left == 4001);
    // A JDK that scales around (0,0): only its answer lands in the window.
    CHECK(JavaPickCaretPx({ 9000, 180, 9002, 204 }, legacy, 3840, 0, 7680, 2160).left == 5921);
    // Neither inside (a caret scrolled out of view): the modern mapping.
    CHECK(JavaPickCaretPx({ 9000, 180, 9002, 204 }, { 9500, 180, 9502, 204 }, 3840, 0, 7680, 2160).left == 9000);
    // Both inside (the monitor at (0,0) gives the same rect anyway): anchored.
    CHECK(JavaPickCaretPx(anchored, { 4100, 180, 4102, 204 }, 3840, 0, 7680, 2160).left == 4001);
}
