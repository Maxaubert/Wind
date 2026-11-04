// swift-tools-version:6.0
// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
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
                .linkedFramework("Metal"),
                .linkedFramework("IOSurface"),
            ]
        ),
        .executableTarget(name: "wind-checks", dependencies: ["WindCore"], path: "Tests/WindCoreChecks"),
    ],
    swiftLanguageModes: [.v5]
)
