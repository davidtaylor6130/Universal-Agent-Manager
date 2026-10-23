// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "UAMKit",
    platforms: [.iOS("26.0"), .macOS(.v15)],
    products: [.library(name: "UAMKit", targets: ["UAMKit"])],
    targets: [
        .target(name: "UAMKit"),
        .testTarget(name: "UAMKitTests", dependencies: ["UAMKit"], path: "Tests")
    ],
    swiftLanguageModes: [.v6]
)
