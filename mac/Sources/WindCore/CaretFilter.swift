// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import Foundation

/// Is the caret where its own focused element is? A port of Windows Wind's `CaretInsideElement`
/// (src/track/track_filter.h, issues #278 and #561): an app can report a caret well outside the
/// field that has focus (Firefox in a zoomed iframe; Electron's fixed rect below Prism's new-folder
/// input on every keystroke), and following it jumps the view away from where the user types.
/// A caret whose centre lies outside the element, plus `slack` for borders and padding, is not
/// trusted. An empty element rect (unknown) trusts the caret. The slack is the Windows value, 8,
/// read here in points (16 px on a Retina screen, so a little more lenient than Windows).
/// Shared cases: tests/fixtures/shared/caret_cases.txt.
public func caretInsideElement(_ caret: Rc, _ elem: Rc, slack: Double = 8) -> Bool {
    if elem.r <= elem.l || elem.b <= elem.t { return true }
    let cx = (caret.l + caret.r) / 2, cy = (caret.t + caret.b) / 2
    return cx >= elem.l - slack && cx <= elem.r + slack && cy >= elem.t - slack && cy <= elem.b + slack
}
