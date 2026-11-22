// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import AppKit
import WindCore

// Saturation (#599, the Windows colour matrix's first step): calmer or stronger colours, 20-150%.
// The display transfer formula that does warmth and dim (ColorFilter) is per channel, so it cannot mix
// channels. Instead each display gets a click-through window above every app holding a CABackdropLayer
// (what NSVisualEffectView uses: WindowServer renders what is below it) with a colour-matrix filter:
// saturationMatrix (WindCore/ColorMath.swift, shared cases with Windows in color_cases.txt). It is applied in
// composition, before the transfer formula, so the order is Windows' (saturation, then warmth and dim).
// - 100% puts nothing on screen: no window at all. Set back to 100 and at quit the windows go.
// - Left out of screenshots (sharingType none) and of screenshot window mode (SkyLight.ignoreForWindowPicker),
//   like warmth, which screenshots do not show either.
// - Render: its capture leaves these windows out and the overlay filters its own picture (DrawZoom.
//   saturationChanged), so it is applied once whether or not the capture honours sharingType. The window
//   stays below the opaque overlay.
// - Backdrop: the overlay's own backdrop gets the filter and these layers hide while it is up (`covered`), so a
//   backdrop never samples another.
// - Transform: macOS's zoom magnifies the composited screen, these windows included; the filter is per pixel.
// The real (hardware) cursor is not affected. Wind's cursor panel and Inspect's crosshair sit below.
final class Saturation {
    static let shared = Saturation()
    /// Just below render's overlay, above every app, the Dock, Wind's cursor panel and the crosshair.
    static let level = DrawZoom.overlayLevel - 1
    private var windows: [NSWindow] = []
    private var backdrops: [CALayer] = []
    private var frames: [CGRect] = []
    private var observing = false
    private var unavailableLogged = false
    /// The applied percent (100 = off).
    private(set) var pct = 100.0
    /// The Backdrop engine's overlay is up and filters its own backdrop: these layers hide meanwhile.
    var covered = false {
        didSet { if covered != oldValue { setHidden() } }
    }

    /// The windows' numbers (render's capture leaves them out).
    var windowIDs: [CGWindowID] { windows.map { CGWindowID($0.windowNumber) } }
    var active: Bool { !windows.isEmpty }

    /// A new filter for the applied percent, nil at 100% (every layer gets its own instance).
    func makeFilter() -> NSObject? { saturationIsNeutral(pct) ? nil : Saturation.filter(pct / 100) }

    func apply(pct newPct: Double) {
        let p = min(150, max(20, newPct))
        if saturationIsNeutral(p) {
            let was = active || !saturationIsNeutral(pct)
            remove()
            pct = 100
            if was { Log.info("saturation 100%: off"); changed() }
            return
        }
        if p == pct && active { return }
        guard Saturation.filter(p / 100) != nil, NSClassFromString("CABackdropLayer") != nil else {
            if !unavailableLogged { Log.error("saturation: no colour filter on this macOS; left at 100%") }
            unavailableLogged = true
            remove(); pct = 100
            return
        }
        pct = p
        if active && frames == NSScreen.screens.map(\.frame) {
            CATransaction.begin(); CATransaction.setDisableActions(true)
            for b in backdrops { b.filters = makeFilter().map { [$0] } }
            CATransaction.commit()
        } else {
            build()
        }
        Log.info("saturation \(Int(p))% on \(windows.count) display(s)")
        changed()
    }

    /// Quit: every window gone and nothing rebuilt by a later display change.
    func stop() { remove(); pct = 100 }

    /// Every window gone (100%, quit, before a rebuild).
    func remove() {
        for w in windows { w.orderOut(nil); w.close() }
        windows = []; backdrops = []; frames = []
    }

    private func build() {
        remove()
        for s in NSScreen.screens {
            guard let cls = NSClassFromString("CABackdropLayer") as? CALayer.Type else { return }
            let w = NSWindow(contentRect: s.frame, styleMask: .borderless, backing: .buffered, defer: false)
            w.level = NSWindow.Level(rawValue: Saturation.level)
            w.ignoresMouseEvents = true
            w.isOpaque = false
            w.backgroundColor = .clear
            w.hasShadow = false
            w.animationBehavior = .none
            w.isReleasedWhenClosed = false
            w.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary, .ignoresCycle]
            w.canHide = false                              // see WindCursor
            w.sharingType = .none                          // screenshots show the real colours, like warmth
            let v = NSView(frame: CGRect(origin: .zero, size: s.frame.size))
            v.wantsLayer = true
            let b = cls.init()
            b.setValue(true, forKey: "windowServerAware"); b.setValue(true, forKey: "enabled")
            b.frame = v.bounds
            b.magnificationFilter = .nearest; b.minificationFilter = .nearest   // 1:1, per pixel
            b.filters = makeFilter().map { [$0] }
            b.isHidden = covered
            v.layer?.addSublayer(b)
            w.contentView = v
            SkyLight.ignoreForWindowPicker(windowNumber: w.windowNumber)   // screenshot window mode picked it
            w.orderFrontRegardless()
            windows.append(w); backdrops.append(b)
        }
        frames = NSScreen.screens.map(\.frame)
        if !observing {
            observing = true
            // A display added, removed or rearranged: one window per display again. The Dock posts this too
            // with nothing changed (DrawZoom.screensChanged): then the frames are equal and nothing happens.
            NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in
                // Wanted, not `active`: built while no display was attached, it has no window yet.
                guard let self, !saturationIsNeutral(self.pct), self.frames != NSScreen.screens.map(\.frame) else { return }
                self.build()
                Log.info("saturation: displays changed, \(self.windows.count) window(s)")
                self.changed()
            }
        }
    }

    /// The filter or the windows changed: render re-filters its picture and leaves the new windows out of its capture.
    private func changed() { DrawZoom.shared.saturationChanged() }

    private func setHidden() {
        CATransaction.begin(); CATransaction.setDisableActions(true)
        for b in backdrops { b.isHidden = covered }
        CATransaction.commit()
    }

    /// A CAFilter "colorMatrix" for saturation 0.2..1.5, nil if this macOS has none.
    static func filter(_ sat01: Double) -> NSObject? {
        guard let cls = NSClassFromString("CAFilter") else { return nil }
        let sel = NSSelectorFromString("filterWithType:")
        let c = cls as AnyObject
        guard c.responds(to: sel), let f = c.perform(sel, with: "colorMatrix")?.takeUnretainedValue() as? NSObject else { return nil }
        var m = caColorMatrix(sat01)
        let v = m.withUnsafeMutableBytes { NSValue(bytes: $0.baseAddress!, objCType: "{CAColorMatrix=ffffffffffffffffffff}") }
        f.setValue(v, forKey: "inputColorMatrix")
        return f
    }
}
