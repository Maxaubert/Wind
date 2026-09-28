#include "focus_track.h"
#include "logging.h"
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

    auto resolve = [&](bool focusChanged) {
        if (!active_.load()) return;
        HWND fg = GetForegroundWindow();
        if (IsOwnOrTooltip(fg)) return;
        RECT rc{};
        // 1. Caret, fastest source first.
        if (wantCaret_.load()) {
            if (Win32Caret(rc)) { publish(TrackKind::Caret, rc.left, rc.top, rc.right, rc.bottom, "win32"); return; }
            IUIAutomationElement* el = nullptr;
            if (uia && SUCCEEDED(uia->GetFocusedElement(&el)) && el) {
                IUIAutomationTextPattern2* tp2 = nullptr;
                if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPattern2Id, __uuidof(IUIAutomationTextPattern2), (void**)&tp2)) && tp2) {
                    BOOL active = FALSE; IUIAutomationTextRange* cr = nullptr;
                    if (SUCCEEDED(tp2->GetCaretRange(&active, &cr)) && cr) {
                        if (active && RangeRect(cr, rc)) { cr->Release(); tp2->Release(); el->Release();
                            publish(TrackKind::Caret, rc.left, rc.top, rc.right, rc.bottom, "uia-caret"); return; }
                        cr->Release();
                    }
                    tp2->Release();
                }
                IUIAutomationTextPattern* tp = nullptr;
                if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern), (void**)&tp)) && tp) {
                    IUIAutomationTextRangeArray* sel = nullptr;
                    if (SUCCEEDED(tp->GetSelection(&sel)) && sel) {
                        int n = 0; sel->get_Length(&n);
                        IUIAutomationTextRange* r0 = nullptr;
                        if (n > 0 && SUCCEEDED(sel->GetElement(0, &r0)) && r0) {
                            bool ok = RangeRect(r0, rc); r0->Release();
                            if (ok) { sel->Release(); tp->Release(); el->Release();
                                publish(TrackKind::Caret, rc.left, rc.top, rc.right, rc.bottom, "uia-selection"); return; }
                        }
                        sel->Release();
                    }
                    tp->Release();
                }
                // 2. Focus: the element's own bounds, only on a real focus change.
                if (focusChanged && wantFocus_.load()) {
                    RECT b{};
                    if (SUCCEEDED(el->get_CurrentBoundingRectangle(&b)) && b.right > b.left && b.bottom > b.top) {
                        el->Release(); publish(TrackKind::Focus, b.left, b.top, b.right, b.bottom, "uia-focus"); return;
                    }
                }
                el->Release();
            }
        } else if (focusChanged && wantFocus_.load() && uia) {
            IUIAutomationElement* el = nullptr; RECT b{};
            if (SUCCEEDED(uia->GetFocusedElement(&el)) && el) {
                if (SUCCEEDED(el->get_CurrentBoundingRectangle(&b)) && b.right > b.left && b.bottom > b.top)
                    publish(TrackKind::Focus, b.left, b.top, b.right, b.bottom, "uia-focus");
                el->Release();
            }
        }
    };

    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == kWakeMsg) {
            if (m.wParam == EVENT_OBJECT_FOCUS || m.wParam == EVENT_SYSTEM_FOREGROUND || m.wParam == EVENT_SYSTEM_MENUPOPUPSTART) pendingFocus = true;
            else pendingCaret = true;
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
