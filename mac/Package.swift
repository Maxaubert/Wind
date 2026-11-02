// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "WindMac",
    platforms: [.macOS(.v14)],
    targets: [
        // Pure logic (no AppKit, no private APIs): everything the tests cover.
        .target(name: "WindCore"),
        .executableTarget(
            name: "Wind",
            dependencies: ["WindCore"],
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("ApplicationServices"),
                .linkedFramework("CoreGraphics"),
                .linkedFramework("ServiceManagement"),
                .linkedFramework("IOKit"),
                .linkedFramework("ScreenCaptureKit"),
            ]
        ),
        .executableTarget(name: "wind-checks", dependencies: ["WindCore"], path: "Tests/WindCoreChecks"),
    ],
    swiftLanguageModes: [.v5]
)
