#pragma once
// Sanity filters for what the focus/caret watcher reports (issue #278). Pure; tests/test_track_filter.cpp.
namespace wind {

struct TrackBox { long l, t, r, b; };

// Is the caret where its own focused element is? Firefox inside a zoomed iframe (a Claude artifact at
// high Ctrl+ page zoom, field 2026-09-29) reports the caret through BOTH GetGUIThreadInfo and UIA well
// outside the focused input (input 2226,997 798x74, caret 3097,1226 1x118), so following it showed
// empty page. A caret whose centre lies outside the element (plus a small slack for borders and
// padding) is not trusted. An empty element rect (unknown) trusts the caret.
inline bool CaretInsideElement(const TrackBox& caret, const TrackBox& elem, long slack = 8) {
    if (elem.r <= elem.l || elem.b <= elem.t) return true;
    const long cx = (caret.l + caret.r) / 2, cy = (caret.t + caret.b) / 2;
    return cx >= elem.l - slack && cx <= elem.r + slack && cy >= elem.t - slack && cy <= elem.b + slack;
}

// Is a focus rect a CONTAINER (the page, a pane, the window) rather than a control someone moved to?
// Leaving a text area moves focus to the whole document, and centring on it dropped the view to the
// middle of the page (field 2026-09-29: 3400x1912 on a 3840x2160 monitor). Half the monitor's area,
// counting only the part on the monitor, is the line.
inline bool IsContainerFocus(const TrackBox& rc, long monL, long monT, long monR, long monB) {
    const long l = rc.l > monL ? rc.l : monL, r = rc.r < monR ? rc.r : monR;
    const long t = rc.t > monT ? rc.t : monT, b = rc.b < monB ? rc.b : monB;
    if (r <= l || b <= t) return false;
    const double area = (double)(r - l) * (double)(b - t);
    const double mon = (double)(monR - monL) * (double)(monB - monT);
    return mon > 0 && area >= 0.5 * mon;
}
}  // namespace wind
