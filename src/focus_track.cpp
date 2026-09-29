#include "focus_track.h"
#include "logging.h"
#include "track_filter.h"
#include <windows.h>
#include <objbase.h>
#include <oleacc.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <string>
#pragma comment(lib, "oleacc.lib")
namespace wind {

static const UINT kWakeMsg = WM_APP + 0x61;       // an event arrived: resolve after coalescing
static const UINT_PTR kCoalesceTimer = 1, kPollTimer = 2;
static FocusTracker* g_self = nullptr;            // WinEvent callbacks have no context pointer

struct FocusTrackImpl {
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
        // The zero-element array is still a real SAFEARRAY allocation; destroy it before sa is
        // reassigned below, or it leaks on every blinking-caret resolve.
        SafeArrayDestroy(sa);
        sa = nullptr;
        IUIAutomationTextRange* wide = nullptr;
        if (SUCCEEDED(range->Clone(&wide)) && wide) {
            if (SUCCEEDED(wide->ExpandToEnclosingUnit(TextUnit_Character)) &&
                SUCCEEDED(wide->GetBoundingRectangles(&sa)) && sa && sa->rgsabound[0].cElements >= 4 &&
                SUCCEEDED(SafeArrayAccessData(sa, (void**)&d))) {
                out = { (LONG)d[0], (LONG)d[1], (LONG)d[0] + 2, (LONG)(d[1] + d[3]) };
                ok = d[3] > 0;
                SafeArrayUnaccessData(sa);
            }
            wide->Release();
        }
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
    if (on && !was) { const unsigned long t = tid_.load(); if (t) PostThreadMessageW(t, kWakeMsg, 0, 0); }
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
    HWINEVENTHOOK h4 = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, FocusTrackImpl::OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(nullptr, kPollTimer, 16, nullptr);   // 60 Hz backstop, work only while active
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

    // Caret, fastest source first. Releases everything it acquires.
    auto findCaret = [&](IUIAutomationElement* el, RECT& rc, const char*& src) -> bool {
        if (Win32Caret(rc)) { src = "win32"; return true; }
        if (!el) return false;
        bool ok = false;
        IUIAutomationTextPattern2* tp2 = nullptr;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPattern2Id, __uuidof(IUIAutomationTextPattern2), (void**)&tp2)) && tp2) {
            BOOL active = FALSE; IUIAutomationTextRange* cr = nullptr;
            if (SUCCEEDED(tp2->GetCaretRange(&active, &cr)) && cr) {
                if (active && RangeRect(cr, rc)) { ok = true; src = "uia-caret"; }
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
                    if (RangeRect(r0, rc)) { ok = true; src = "uia-selection"; }
                    r0->Release();
                }
                sel->Release();
            }
            tp->Release();
        }
        return ok;
    };

    auto resolve = [&](bool focusChanged) {
        if (!active_.load()) return;
        HWND fg = GetForegroundWindow();
        if (IsOwnOrTooltip(fg)) return;
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
            if (findCaret(el, rc, src)) {
                if (caretGen != focusGen) {
                    caretGen = focusGen; lastCaret = rc;                       // baseline, not followed
                    if (log_.load()) wind::Log(wind::LogLevel::Info, "track", "caret baseline via %s: %ld,%ld", src, rc.left, rc.top);
                } else if (!EqualRect(&rc, &lastCaret)) {
                    lastCaret = rc;
                    // A caret outside its own element is a bad report, not a place to look (Firefox
                    // in a zoomed iframe, issue #278): the view stays where it is.
                    if (!CaretInsideElement({ rc.left, rc.top, rc.right, rc.bottom }, { b.left, b.top, b.right, b.bottom })) {
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
        if (m.message == kWakeMsg) {
            if (m.wParam == EVENT_OBJECT_FOCUS || m.wParam == EVENT_SYSTEM_FOREGROUND || m.wParam == EVENT_SYSTEM_MENUPOPUPSTART) {
                pendingFocus = true;
                ++focusGen;          // at ARRIVAL: see the baseline note above resolve
            } else if (m.wParam == 0) {
                pendingCaret = true;
                ++focusGen;          // (re)activation: the caret found now is a baseline too
            } else {
                pendingCaret = true;
            }
            if (!coalesce) coalesce = SetTimer(nullptr, kCoalesceTimer, 30, nullptr);   // NVDA's ~30 ms
        } else if (m.message == WM_TIMER && m.wParam == coalesce && coalesce) {
            KillTimer(nullptr, coalesce); coalesce = 0;
            resolve(pendingFocus); pendingFocus = pendingCaret = false;
        } else if (m.message == WM_TIMER) {
            if (active_.load() && wantCaret_.load()) resolve(false);                    // backstop poll
        }
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    KillTimer(nullptr, kPollTimer);
    for (HWINEVENTHOOK h : { h1, h2, h3, h4 }) if (h) UnhookWinEvent(h);
    if (uia && fh) uia->RemoveFocusChangedEventHandler(fh);
    if (fh) fh->Release();
    if (uia) uia->Release();
    CoUninitialize();
    tid_ = 0;
}
}  // namespace wind
