#include "doctest.h"
#include "../src/java_bridge_util.h"
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
