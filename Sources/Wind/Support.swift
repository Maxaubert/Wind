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
        let u = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/Wind")
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
        // Keep one previous log; start fresh each launch.
        if FileManager.default.fileExists(atPath: p) {
            let old = Paths.logDir.appendingPathComponent("wind.prev.log")
            try? FileManager.default.removeItem(at: old)
            try? FileManager.default.moveItem(at: Paths.log, to: old)
        }
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
/// zoom back to 1x and the display colours back, so a crash never leaves the screen zoomed.
enum Watchdog {
    static func spawn() {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: CommandLine.arguments[0])
        p.arguments = ["--watchdog", String(getpid())]
        do { try p.run() } catch { Log.error("watchdog spawn failed: \(error)") }
    }

    static func run(parent: pid_t) -> Never {
        signal(SIGTERM, SIG_IGN)
        while kill(parent, 0) == 0 { usleep(200_000) }
        if SkyLight.isZoomed { SkyLight.reset() }
        CGDisplayRestoreColorSyncSettings()
        exit(0)
    }
}
