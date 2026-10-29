import Foundation

// A tiny test runner: Command Line Tools ship neither XCTest nor the swift-testing macros.
// `swift run wind-checks` exits 0 when every check passes.

var failures = 0
var current = ""

func expect(_ ok: Bool, file: StaticString = #fileID, line: UInt = #line) {
    if !ok { failures += 1; print("FAIL \(current) at \(file):\(line)") }
}

func run(_ name: String, _ body: () throws -> Void) {
    current = name
    do { try body() } catch { failures += 1; print("FAIL \(name): threw \(error)") }
}

let z = ZoomTests()
run("holdZoomsInAtTheBaseRate", z.holdZoomsInAtTheBaseRate)
run("releaseEasesOutThenStops", z.releaseEasesOutThenStops)
run("clampsAtMax", z.clampsAtMax)
run("zoomOutStopsAtOne", z.zoomOutStopsAtOne)
run("wheelGlidesToTarget", z.wheelGlidesToTarget)
run("bothHeldFreezes", z.bothHeldFreezes)
run("quickZoomToggles", z.quickZoomToggles)
let v = ViewTests()
run("centreClampsToScreen", v.centreClampsToScreen)
run("edgePanMovesOnlyPastTheBand", v.edgePanMovesOnlyPastTheBand)
run("springConverges", v.springConverges)
run("trackTargetRejectsBogusRects", v.trackTargetRejectsBogusRects)
let l = LockTests()
run("deltasWithoutTravelLock", l.deltasWithoutTravelLock)
run("normalMovementNeverLocks", l.normalMovementNeverLocks)
run("pinnedAtEdgeIsNotALock", l.pinnedAtEdgeIsNotALock)
run("zoomedTravelIsJudgedAt1x", l.zoomedTravelIsJudgedAt1x)
let o = OwnerTests()
run("typedCaretTakesTheViewAndMouseTakesItBack", o.typedCaretTakesTheViewAndMouseTakesItBack)
run("caretWithoutAKeyIsIgnored", o.caretWithoutAKeyIsIgnored)
let b = BindTests()
run("parseAndFormat", b.parseAndFormat)
run("rules", b.rules)
run("routerHoldsAndSwallowsBalanced", b.routerHoldsAndSwallowsBalanced)
run("timeoutStopsButKeepsSwallowing", b.timeoutStopsButKeepsSwallowing)
run("plusKeyBinds", b.plusKeyBinds)
let c = ConfigTests()
run("partialFileKeepsDefaults", c.partialFileKeepsDefaults)
run("clampsValues", c.clampsValues)
run("wrongTypeDropsOnlyThatKey", c.wrongTypeDropsOnlyThatKey)
run("defaultBindsAreAllValid", c.defaultBindsAreAllValid)

print(failures == 0 ? "all checks passed" : "\(failures) check(s) failed")
exit(failures == 0 ? 0 : 1)
