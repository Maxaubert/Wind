#include "focus_track.h"
#include "logging.h"
#include "track_filter.h"
#include "caret_rect.h"   // trim tall UIA caret rects (#337)
#include "java_bridge.h"
#include "java_bridge_util.h"
#include <windows.h>
#include <objbase.h>
#include <oleacc.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <dwmapi.h>
#include <string>
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "dwmapi.lib")
namespace wind {

static const UINT kWakeMsg = WM_APP + 0x61;       // an event arrived: resolve after coalescing
static const UINT kActiveMsg = WM_APP + 0x62;     // active_ changed (wParam = new value): retune (#71)
static const UINT_PTR kCoalesceTimer = 1;
static FocusTracker* g_self = nullptr;            // WinEvent callbacks have no context pointer

// The shell's input panels (emoji picker, clipboard history, touch keyboard) are hosted by
// TextInputHost.exe and composed by the shell ABOVE every app window, so no band Wind can create
// covers them (issue #283, measured 2026-09-29). They never change as windows either: the only
// reliable signal is TextInputHost's "IME" window, uncloaked while a panel shows and cloaked when it
// closes (every open/close in the field recording matched). "IME" is a common class name (every GUI
// thread has one), so the owning process is checked too.
static bool IsShellPanelWindow(HWND h) {
    wchar_t cls[16] = {};
    if (!GetClassNameW(h, cls, 16) || wcscmp(cls, L"IME") != 0) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
    bool yes = false;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        yes = _wcsicmp(base ? base + 1 : path, L"TextInputHost.exe") == 0;
    }
    CloseHandle(p);
    return yes;
}

struct FocusTrackImpl {
    static void SetPanel(bool open, HWND h) {
        if (!g_self) return;
        g_self->panelHwnd_.store(open ? h : nullptr);
        if (g_self->panelOpen_.exchange(open) != open)
            wind::Log(wind::LogLevel::Info, "track", "shell input panel %s", open ? "open" : "closed");
    }
    static void CALLBACK OnCloak(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG, DWORD, DWORD) {
        if (!g_self || !h || obj != OBJID_WINDOW) return;
        if (!IsShellPanelWindow(h)) return;
        SetPanel(ev == EVENT_OBJECT_UNCLOAKED, h);
    }
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD ev, HWND, LONG obj, LONG, DWORD, DWORD) {
        if (!g_self || !g_self->active_.load()) return;
        if (ev == EVENT_OBJECT_LOCATIONCHANGE && obj != OBJID_CARET) return;
        PostThreadMessageW(g_self->tid_.load(), kWakeMsg, (WPARAM)ev, 0);
    }
};

// UIA focus-changed handler: just wakes the thread (resolution happens there, coalesced).
class FocusHandler : public IUIAutomationFocusChangedEventHandler {
    LONG refs_ = 1;
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IUIAutomationFocusChangedEventHandler)) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement*) override {
        if (g_self && g_self->active_.load()) PostThreadMessageW(g_self->tid_.load(), kWakeMsg, (WPARAM)EVENT_OBJECT_FOCUS, 0);
        return S_OK;
    }
};

static bool IsOwnOrTooltip(HWND h) {
    if (!h) return true;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return true;
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
    return wcscmp(cls, L"tooltips_class32") == 0 || wcscmp(cls, L"Xaml_WindowedPopupClass") == 0;
}

// A Gecko browser window (Firefox and every fork share this class).
static bool IsGeckoWindow(HWND h) {
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
    return wcscmp(cls, L"MozillaWindowClass") == 0;
}

// A Java top-level window (IntelliJ, PyCharm, ...): its caret comes from the Java Access Bridge.
static bool IsJavaWindow(HWND h) {
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
    return IsJavaWindowClass(cls);
}

// Classic Win32 caret of the foreground thread, in screen px. False when there is none.
static bool Win32Caret(RECT& out) {
    HWND fg = GetForegroundWindow();
    if (IsOwnOrTooltip(fg)) return false;
    GUITHREADINFO gi{ sizeof(gi) };
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gi) || !gi.hwndCaret) return false;
    RECT rc = gi.rcCaret;
    if (rc.right <= rc.left && rc.bottom <= rc.top) return false;
    POINT a{ rc.left, rc.top }, b{ rc.right, rc.bottom };
    if (!ClientToScreen(gi.hwndCaret, &a) || !ClientToScreen(gi.hwndCaret, &b)) return false;
    out = { a.x, a.y, b.x, b.y };
    return true;
}

// The MSAA system caret object (OBJID_CARET) of the focused window: Chromium/Electron maintain it for
// screen magnifiers even where their UIA caret is only a line (#341). Rejected when empty or line-wide.
static bool MsaaCaret(RECT& out) {
    HWND fg = GetForegroundWindow();
    if (!fg || IsOwnOrTooltip(fg)) return false;
    GUITHREADINFO gi{ sizeof(gi) };
    HWND h = fg;
    if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gi) && gi.hwndFocus) h = gi.hwndFocus;
    IAccessible* acc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(h, (DWORD)OBJID_CARET, IID_IAccessible, (void**)&acc)) || !acc) return false;
    VARIANT self; VariantInit(&self); self.vt = VT_I4; self.lVal = CHILDID_SELF;
    long x = 0, y = 0, w = 0, hh = 0;
    const bool got = SUCCEEDED(acc->accLocation(&x, &y, &w, &hh, self));
    acc->Release();
    if (!got || hh <= 0 || (x == 0 && y == 0)) return false;
    out = { x, y, x + (w > 1 ? w : 2), y + hh };
    return !wind::IsLineWideCaret(out.left, out.top, out.right, out.bottom);
}

// The caret as the left edge of the character it sits on: the range widened by one character.
static bool CharRect(IUIAutomationTextRange* range, RECT& out) {
    bool ok = false;
    IUIAutomationTextRange* wide = nullptr;
    if (SUCCEEDED(range->Clone(&wide)) && wide) {
        SAFEARRAY* sa = nullptr;
        double* d = nullptr;
        if (SUCCEEDED(wide->ExpandToEnclosingUnit(TextUnit_Character)) &&
            SUCCEEDED(wide->GetBoundingRectangles(&sa)) && sa && sa->rgsabound[0].cElements >= 4 &&
            SUCCEEDED(SafeArrayAccessData(sa, (void**)&d))) {
            out = { (LONG)d[0], (LONG)d[1], (LONG)d[0] + 2, (LONG)(d[1] + d[3]) };
            ok = d[3] > 0 && d[2] < d[3] * wind::kWideCaretRatio;   // a character, not the whole line again
            SafeArrayUnaccessData(sa);
        }
        if (sa) SafeArrayDestroy(sa);
        wide->Release();
    }
    return ok;
}

// An empty (collapsed) range: a caret, not a selection. Unknown counts as empty (review #349: only a
// caret may be replaced by the #341 line-wide fallback; a long keyboard selection is simply wide).
static bool RangeIsEmpty(IUIAutomationTextRange* range) {
    int cmp = 1;
    if (SUCCEEDED(range->CompareEndpoints(TextPatternRangeEndpoint_Start, range, TextPatternRangeEndpoint_End, &cmp)))
        return cmp == 0;
    BSTR text = nullptr;
    if (SUCCEEDED(range->GetText(1, &text))) {
        const bool empty = !text || SysStringLen(text) == 0;
        if (text) SysFreeString(text);
        return empty;
    }
    return true;
}

static bool RangeRect(IUIAutomationTextRange* range, RECT& out) {
    SAFEARRAY* sa = nullptr;
    if (FAILED(range->GetBoundingRectangles(&sa)) || !sa) return false;
    bool ok = false;
    double* d = nullptr;
    LONG n = sa->rgsabound[0].cElements;
    if (n >= 4 && SUCCEEDED(SafeArrayAccessData(sa, (void**)&d))) {
        out = { (LONG)d[0], (LONG)d[1], (LONG)(d[0] + (d[2] > 1 ? d[2] : 1)), (LONG)(d[1] + d[3]) };
        ok = d[3] > 0;
        SafeArrayUnaccessData(sa);
    } else if (n == 0) {
        // An empty caret range has no rectangle: widen it by one character, then use its left edge.
        // The zero-element array is still a real SAFEARRAY allocation; destroy it here, or it leaks on
        // every blinking-caret resolve.
        SafeArrayDestroy(sa);
        sa = nullptr;
        ok = CharRect(range, out);
    }
    if (sa) SafeArrayDestroy(sa);
    return ok;
}

bool FocusTracker::start() {
    if (th_.joinable()) return true;
    g_self = this;
    th_ = std::thread([this] { run(); });
    return true;
}
void FocusTracker::stop() {
    const unsigned long t = tid_.load();
    if (t) PostThreadMessageW(t, WM_QUIT, 0, 0);
    if (th_.joinable()) th_.join();
    if (g_self == this) g_self = nullptr;
}
void FocusTracker::setActive(bool on, bool wantCaret, bool wantFocus, bool log) {
    wantCaret_ = wantCaret; wantFocus_ = wantFocus; log_ = log;
    const bool was = active_.exchange(on);
    const unsigned long t = tid_.load();
    if (t && on != was) PostThreadMessageW(t, kActiveMsg, on ? 1 : 0, 0);   // hooks + poll rate
    if (on && !was && t) PostThreadMessageW(t, kWakeMsg, 0, 0);
}
void FocusTracker::publish(TrackKind k, double l, double t, double r, double b, const char* src) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (snap_.kind == k && snap_.l == l && snap_.t == t && snap_.r == r && snap_.b == b) return;  // unchanged
        snap_ = { k, ++seq_, l, t, r, b };
    }
    if (log_) wind::Log(wind::LogLevel::Info, "track", "%s via %s: %.0f,%.0f %.0fx%.0f",
                        k == TrackKind::Caret ? "caret" : "focus", src, l, t, r - l, b - t);
}

void FocusTracker::run() {
    tid_ = GetCurrentThreadId();
    MSG m; PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // make the queue exist
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
    FocusHandler* fh = nullptr;
    if (uia) { fh = new FocusHandler(); if (FAILED(uia->AddFocusChangedEventHandler(nullptr, fh))) { fh->Release(); fh = nullptr; } }
    HWINEVENTHOOK h1 = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FocusTrackImpl::OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK h2 = SetWinEventHook(EVENT_SYSTEM_MENUPOPUPSTART, EVENT_SYSTEM_MENUPOPUPSTART, nullptr, FocusTrackImpl::OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK h3 = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, FocusTrackImpl::OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // LOCATIONCHANGE fires for every moving window and caret system-wide: installed only while the
    // tracker is active (zoomed), never at 1x (#71). The 16 ms caret backstop likewise; while idle a
    // 250 ms timer only re-checks a shell panel marked open.
    HWINEVENTHOOK h4 = nullptr;
    UINT_PTR pollTimer = 0;
    auto retune = [&](bool on) {
        if (on && !h4)
            h4 = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, FocusTrackImpl::OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!on && h4) { UnhookWinEvent(h4); h4 = nullptr; }
        if (pollTimer) KillTimer(nullptr, pollTimer);
        pollTimer = SetTimer(nullptr, 0, on ? 16 : 250, nullptr);
    };
    HWINEVENTHOOK h5 = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, FocusTrackImpl::OnCloak, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // A panel already open when we start raised its UNCLOAKED before the hook existed (review #284).
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        DWORD cloaked = 1;
        if (IsShellPanelWindow(h) && SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && !cloaked) {
            FocusTrackImpl::SetPanel(true, h);
            return FALSE;
        }
        return TRUE;
    }, 0);
    retune(active_.load());   // an activation that raced thread start-up is picked up here
    bool pendingFocus = false, pendingCaret = false;
    UINT_PTR coalesce = 0;

    // FOLLOW ONLY WHAT THE KEYBOARD MOVES (field test 2026-09-29). Landing in a field, by Tab or by a
    // click, reports a caret too (at the END of a filled field), and following it jumped the view
    // away from what the user was looking at. So the first caret position after any focus change is
    // only a BASELINE; a caret is published when it moves within the same focus, i.e. typing or the
    // arrow keys. focusGen advances the moment a focus event ARRIVES (not when it is resolved), so
    // the 60 Hz backstop poll cannot publish the new field's caret in the 30 ms before that.
    unsigned focusGen = 0, caretGen = ~0u;
    RECT lastCaret{};
    wind::CaretLineState caretLine;   // one-line caret height in the current focus (#337)
    wind::CaretHoldState caretHold;   // last caret line, for mid-scroll Enter reports (#337)

    // Java apps (issue #281): the bridge is asked only when something may have moved (a bridge caret
    // or focus callback, or any tracker wake), never by the 60 Hz poll, which reuses the last answer.
    // A bridge call is a round trip into the Java app's own UI thread, so polling it would load the app.
    JavaBridge jab;
    bool javaDirty = true;
    HWND javaWnd = nullptr;
    bool javaHave = false;
    RECT javaCaret{};
    ULONGLONG javaRetryAt = 0;              // a failed read is retried at most every 250 ms
    unsigned javaEvents = 0;

    // #341 line-wide fallback, cached per focus (review #349): the character at the caret or the MSAA
    // system caret is re-asked only on a real caret/focus event, a changed line rect, or at most every
    // 250 ms from the 60 Hz poll, and "caret ignored" is logged only when the outcome changes.
    struct LineWideCache {
        unsigned gen = ~0u;          // focusGen it belongs to
        RECT line{};                 // the line-wide rect it answered
        bool ok = false;
        RECT rc{};
        const char* src = "";
        ULONGLONG retryAt = 0;
    } lineWide;

    // #341: a UIA caret that is still the whole line (VS Code / Electron: 263,1752 3330x44, and a 3330x3
    // strip) says nothing about where the caret is. The character at the caret, when the editor exposes
    // it, gives the real x; else Chromium's system caret object for screen magnifiers (OBJID_CARET); else
    // no caret rather than a guess. Only for a collapsed range: a real selection keeps its own rect.
    auto fixLineWide = [&](IUIAutomationTextRange* range, RECT& rc, const char*& src, bool fromPoll) -> bool {
        const ULONGLONG now = GetTickCount64();
        if (lineWide.gen == focusGen && EqualRect(&lineWide.line, &rc) && fromPoll && now < lineWide.retryAt) {
            if (lineWide.ok) { rc = lineWide.rc; src = lineWide.src; }
            return lineWide.ok;
        }
        RECT out{}; const char* outSrc = src; bool ok = false;
        if (CharRect(range, out)) ok = true;
        else if (MsaaCaret(out)) { ok = true; outSrc = "msaa-caret"; }
        const bool changed = lineWide.gen != focusGen || ok != lineWide.ok;
        if (!ok && changed && log_.load())
            wind::Log(wind::LogLevel::Info, "track", "caret ignored (whole line %ldx%ld, no system caret)",
                      rc.right - rc.left, rc.bottom - rc.top);
        lineWide.gen = focusGen; lineWide.line = rc; lineWide.ok = ok; lineWide.rc = out; lineWide.src = outSrc;
        lineWide.retryAt = now + 250;
        if (ok) { rc = out; src = outSrc; }
        return ok;
    };

    // Caret, fastest source first. Releases everything it acquires.
    auto findCaret = [&](IUIAutomationElement* el, RECT& rc, const char*& src, bool fromPoll) -> bool {
        if (Win32Caret(rc)) { src = "win32"; return true; }
        if (!el) return false;
        bool ok = false;
        IUIAutomationTextPattern2* tp2 = nullptr;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPattern2Id, __uuidof(IUIAutomationTextPattern2), (void**)&tp2)) && tp2) {
            BOOL active = FALSE; IUIAutomationTextRange* cr = nullptr;
            if (SUCCEEDED(tp2->GetCaretRange(&active, &cr)) && cr) {
                if (active && RangeRect(cr, rc)) {
                    ok = true; src = "uia-caret";
                    if (wind::IsLineWideCaret(rc.left, rc.top, rc.right, rc.bottom))   // a caret range is collapsed
                        ok = fixLineWide(cr, rc, src, fromPoll);
                }
                cr->Release();
            }
            tp2->Release();
        }
        if (ok) return true;
        IUIAutomationTextPattern* tp = nullptr;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern), (void**)&tp)) && tp) {
            IUIAutomationTextRangeArray* sel = nullptr;
            if (SUCCEEDED(tp->GetSelection(&sel)) && sel) {
                int n = 0; sel->get_Length(&n);
                IUIAutomationTextRange* r0 = nullptr;
                if (n > 0 && SUCCEEDED(sel->GetElement(0, &r0)) && r0) {
                    if (RangeRect(r0, rc)) {
                        ok = true; src = "uia-selection";
                        if (wind::IsLineWideCaret(rc.left, rc.top, rc.right, rc.bottom) && RangeIsEmpty(r0))
                            ok = fixLineWide(r0, rc, src, fromPoll);
                    }
                    r0->Release();
                }
                sel->Release();
            }
            tp->Release();
        }
        return ok;
    };

    auto resolve = [&](bool focusChanged, bool fromPoll) {
        if (!active_.load()) return;
        HWND fg = GetForegroundWindow();
        if (IsOwnOrTooltip(fg)) return;
        // Java app: the bridge is the only caret source (its Win32/UIA views have no caret).
        const bool java = wantCaret_.load() && IsJavaWindow(fg) && jab.ensure(fg, tid_.load(), kWakeMsg, log_.load());
        if (java) {
            if (fg != javaWnd) { javaWnd = fg; javaDirty = true; }
            const ULONGLONG now = GetTickCount64();
            // Only after a Java event or window switch (javaDirty), or one retry per 250 ms after a
            // failed read: unrelated system-wide wakes must not become Java round trips (review #281).
            if (javaDirty || (!javaHave && now >= javaRetryAt)) {
                javaHave = jab.caret(fg, javaCaret);
                javaDirty = false;
                if (!javaHave) javaRetryAt = now + 250;
                if (log_.load()) wind::Log(wind::LogLevel::Info, "track", "java read %s (poll=%d events=%u): %ld,%ld %ldx%ld",
                                           javaHave ? "ok" : "none", (int)fromPoll, javaEvents, javaCaret.left, javaCaret.top,
                                           javaCaret.right - javaCaret.left, javaCaret.bottom - javaCaret.top);
            }
        }
        IUIAutomationElement* el = nullptr;
        if (uia) uia->GetFocusedElement(&el);
        RECT b{};                                   // the focused element's bounds (empty = unknown)
        if (el && FAILED(el->get_CurrentBoundingRectangle(&b))) b = RECT{};
        // 1. A focus change: follow the focused control (if wanted). Its caret becomes the baseline.
        //    A container-sized focus (the page after leaving a text box, a pane, the window) is not
        //    something the user moved to, so it is skipped (issue #278).
        if (focusChanged && wantFocus_.load() && el && b.right > b.left && b.bottom > b.top) {
            MONITORINFO mi{ sizeof(mi) };
            const bool haveMon = GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi) != 0;
            if (haveMon && IsContainerFocus({ b.left, b.top, b.right, b.bottom },
                                            mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom)) {
                if (log_.load()) wind::Log(wind::LogLevel::Info, "track", "focus skipped (container): %ld,%ld %ldx%ld",
                                           b.left, b.top, b.right - b.left, b.bottom - b.top);
            } else {
                publish(TrackKind::Focus, b.left, b.top, b.right, b.bottom, "uia-focus");
            }
        }
        // 2. The caret: published only when it moved within the same focus.
        if (wantCaret_.load()) {
            RECT rc{}; const char* src = "";
            const bool found = java ? (javaHave ? (rc = javaCaret, src = "java", true) : false) : findCaret(el, rc, src, fromPoll);
            if (found) {
                // #337: a caret rect that also spans blank lines above (Chromium web editors, both its
                // UIA and Win32 carets) is trimmed to one line at its bottom, the real caret line. The
                // line height is per focus; Java carets come from the bridge and are left alone.
                if (caretGen != focusGen) { caretLine = wind::CaretLineState{}; caretHold = wind::CaretHoldState{}; }
                if (!java) {
                    const LONG rawTop = rc.top;
                    int top = (int)rc.top;
                    wind::TrimTallCaret(top, (int)rc.bottom, caretLine);
                    rc.top = top;
                    if (rc.top != rawTop && log_.load())
                        wind::Log(wind::LogLevel::Info, "track", "caret trimmed (tall %ld px rect) to %ld px line", rc.bottom - rawTop, rc.bottom - rc.top);
                    // A new line reported part-way through the page's scroll stays on the current line.
                    int bot = (int)rc.bottom; top = (int)rc.top;
                    if (wind::HoldMidScrollCaret((int)rc.left, top, bot, caretLine.lineH, caretHold) && log_.load())
                        wind::Log(wind::LogLevel::Info, "track", "caret held on its line (mid-scroll report %ld-%ld)", rc.top, rc.bottom);
                    rc.top = top; rc.bottom = bot;
                }
                if (caretGen != focusGen) {
                    caretGen = focusGen; lastCaret = rc;                       // baseline, not followed
                    if (log_.load()) wind::Log(wind::LogLevel::Info, "track", "caret baseline via %s: %ld,%ld", src, rc.left, rc.top);
                } else if (!EqualRect(&rc, &lastCaret)) {
                    lastCaret = rc;
                    // In a Gecko browser (Firefox, Zen, LibreWolf...: one window class), a caret
                    // outside its own element is a bad report, not a place to look (a zoomed iframe,
                    // issue #278; its UIA caret was wrong too, so there is no source to fall back
                    // to): the view stays where it is. Other apps are unchanged, so no case that
                    // worked before can lose tracking to this rule.
                    if (IsGeckoWindow(fg) &&
                        !CaretInsideElement({ rc.left, rc.top, rc.right, rc.bottom }, { b.left, b.top, b.right, b.bottom })) {
                        if (log_.load()) wind::Log(wind::LogLevel::Info, "track", "caret skipped (outside its element %ld,%ld %ldx%ld) via %s: %ld,%ld",
                                                   b.left, b.top, b.right - b.left, b.bottom - b.top, src, rc.left, rc.top);
                    } else {
                        publish(TrackKind::Caret, rc.left, rc.top, rc.right, rc.bottom, src);
                    }
                }
            }
        }
        if (el) el->Release();
    };

    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == kActiveMsg) { retune(m.wParam != 0); continue; }
        if (m.message == kWakeMsg) {
            if (m.wParam == EVENT_OBJECT_FOCUS || m.wParam == EVENT_SYSTEM_FOREGROUND || m.wParam == EVENT_SYSTEM_MENUPOPUPSTART) {
                pendingFocus = true;
                ++focusGen;          // at ARRIVAL: see the baseline note above resolve
            } else if (m.wParam == 0) {
                pendingCaret = true;
                ++focusGen;          // (re)activation: the caret found now is a baseline too
            } else if (m.wParam == JavaBridge::kJavaFocus) {
                pendingFocus = true; javaDirty = true; ++javaEvents;
                ++focusGen;          // a Java focus change: its caret is a baseline, like any other
            } else if (m.wParam == JavaBridge::kJavaCaret) {
                pendingCaret = true; javaDirty = true; ++javaEvents;
            } else {
                pendingCaret = true;
            }
            if (!coalesce) coalesce = SetTimer(nullptr, kCoalesceTimer, 30, nullptr);   // NVDA's ~30 ms
        } else if (m.message == WM_TIMER && m.wParam == coalesce && coalesce) {
            KillTimer(nullptr, coalesce); coalesce = 0;
            resolve(pendingFocus, false); pendingFocus = pendingCaret = false;
        } else if (m.message == WM_TIMER) {
            // A missed close (TextInputHost restarted, an event dropped) must not leave the real
            // pointer on for good: re-check the remembered panel window while it is marked open.
            if (panelOpen_.load()) {
                HWND ph = static_cast<HWND>(panelHwnd_.load());
                DWORD cloaked = 0;
                if (!ph || !IsWindow(ph) ||
                    (SUCCEEDED(DwmGetWindowAttribute(ph, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked))
                    FocusTrackImpl::SetPanel(false, nullptr);
            }
            if (active_.load() && wantCaret_.load()) resolve(false, true);              // backstop poll
        }
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    if (pollTimer) KillTimer(nullptr, pollTimer);
    for (HWINEVENTHOOK h : { h1, h2, h3, h4, h5 }) if (h) UnhookWinEvent(h);
    if (uia && fh) uia->RemoveFocusChangedEventHandler(fh);
    if (fh) fh->Release();
    if (uia) uia->Release();
    CoUninitialize();
    tid_ = 0;
}
}  // namespace wind
