import Foundation
import AppKit

enum Paths {
    static let support: URL = {
        let u = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Wind")
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }()
    static let config = support.appendingPathComponent("config.json")
    /// Test and diagnostics channel: write a command line here, Wind runs it and writes state.json.
    static let command = support.appendingPathComponent("command")
    static let state = support.appendingPathComponent("state.json")
    static let logDir: URL = {
        // Test builds log apart: their runs rotated the owner's own session log away before it could be
        // read (twice on 2026-10-31).
        #if WIND_TEST_CHANNEL
        let u = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/Wind/test")
        #else
        let u = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/Wind")
        #endif
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }()
    static let log = logDir.appendingPathComponent("wind.log")
}

enum Log {
    static var level = 1
    private static let queue = DispatchQueue(label: "wind.log")
    private static var handle: FileHandle? = {
        let p = Paths.log.path
        // Keep the last five logs (wind.1.log is the previous launch): test runs relaunch Wind
        // several times, and one previous log lost a user's session before it could be read.
        let fm = FileManager.default
        func numbered(_ i: Int) -> URL { Paths.logDir.appendingPathComponent("wind.\(i).log") }
        try? fm.removeItem(at: numbered(5))
        for i in stride(from: 4, through: 1, by: -1) { try? fm.moveItem(at: numbered(i), to: numbered(i + 1)) }
        if fm.fileExists(atPath: p) { try? fm.moveItem(at: Paths.log, to: numbered(1)) }
        try? fm.removeItem(at: Paths.logDir.appendingPathComponent("wind.prev.log"))
        FileManager.default.createFile(atPath: p, contents: nil)
        return FileHandle(forWritingAtPath: p)
    }()
    private static let fmt: DateFormatter = {
        let f = DateFormatter(); f.dateFormat = "HH:mm:ss.SSS"; return f
    }()

    static func info(_ s: @autoclosure () -> String) { if level >= 1 { write(s()) } }
    static func verbose(_ s: @autoclosure () -> String) { if level >= 2 { write(s()) } }
    static func error(_ s: String) { write("ERROR " + s) }

    private static func write(_ s: String) {
        let line = "\(fmt.string(from: Date())) \(s)\n"
        queue.async { handle?.write(Data(line.utf8)) }
    }
}

/// While Wind is alive this process does nothing; when Wind dies (crash, kill -9) it puts the
/// zoom back to 1x, the mice's speed and the display colours back, so a crash never leaves the screen zoomed.
enum Watchdog {
    static func spawn() {
        let p = Process()
        // A copy named WindGuard (scripts/build.sh): `pkill -x Wind` or killall must not take the
        // guard down with Wind, since recovering from exactly that is its job.
        let guardURL = Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/WindGuard")
        p.executableURL = FileManager.default.isExecutableFile(atPath: guardURL.path)
            ? guardURL : URL(fileURLWithPath: CommandLine.arguments[0])
        p.arguments = ["--watchdog", String(getpid())]
        do { try p.run() } catch { Log.error("watchdog spawn failed: \(error)") }
    }

    static func run(parent: pid_t) -> Never {
        signal(SIGTERM, SIG_IGN)
        while kill(parent, 0) == 0 { usleep(200_000) }
        if SkyLight.isZoomed { SkyLight.reset() }
        HIDGain.restoreSaved()
        CGDisplayRestoreColorSyncSettings()
        exit(0)
    }
}
