// swift-tools-version:5.10
import PackageDescription

let package = Package(
    name: "JustPrintAlready",
    platforms: [.macOS(.v14)],
    targets: [
        .target(name: "JPACore"),
        .executableTarget(name: "JustPrintAlready", dependencies: ["JPACore"]),
        .testTarget(name: "JPACoreTests", dependencies: ["JPACore"]),
    ]
)
