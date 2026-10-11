import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

// Preserve the source artwork while filling the square canvas behind it.
func writeIcon(_ image: CGImage, size: Int, destination: URL) throws
{
    let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    guard let context = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8,
        bytesPerRow: size * 4, space: colorSpace, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    else { throw NSError(domain: "UAMIcon", code: 1) }
    let side = CGFloat(size)
    let tile = CGPath(roundedRect: CGRect(x: 0, y: 0, width: side, height: side),
        cornerWidth: side * 0.22, cornerHeight: side * 0.22, transform: nil)
    context.addPath(tile)
    context.setFillColor(CGColor(colorSpace: colorSpace, components: [24.0 / 255, 24.0 / 255, 26.0 / 255, 1])!)
    context.fillPath()
    let scale = side / CGFloat(max(image.width, image.height))
    let width = CGFloat(image.width) * scale
    let height = CGFloat(image.height) * scale
    context.interpolationQuality = .high
    context.draw(image, in: CGRect(x: (side - width) / 2, y: (side - height) / 2, width: width, height: height))
    guard let result = context.makeImage(),
        let output = CGImageDestinationCreateWithURL(destination as CFURL, UTType.png.identifier as CFString, 1, nil)
    else { throw NSError(domain: "UAMIcon", code: 2) }
    CGImageDestinationAddImage(output, result, nil)
    guard CGImageDestinationFinalize(output) else { throw NSError(domain: "UAMIcon", code: 3) }
}

guard CommandLine.arguments.count == 3 else
{
    fatalError("Usage: swift compose_macos_icon.swift source.png output-directory")
}
let source = URL(fileURLWithPath: CommandLine.arguments[1])
let output = URL(fileURLWithPath: CommandLine.arguments[2], isDirectory: true)
guard let decoder = CGImageSourceCreateWithURL(source as CFURL, nil),
    let image = CGImageSourceCreateImageAtIndex(decoder, 0, nil)
else { fatalError("The original artwork could not be read.") }
let iconset = output.appendingPathComponent("app_icon.iconset", isDirectory: true)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
try writeIcon(image, size: 1024, destination: output.appendingPathComponent("app_icon_macos.png"))
for size in [16, 32, 128, 256, 512]
{
    try writeIcon(image, size: size, destination: iconset.appendingPathComponent("icon_\(size)x\(size).png"))
    try writeIcon(image, size: size * 2, destination: iconset.appendingPathComponent("icon_\(size)x\(size)@2x.png"))
}
let pack = Process()
pack.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
pack.arguments = ["--convert", "icns", "--output", output.appendingPathComponent("app_icon.icns").path, iconset.path]
try pack.run()
pack.waitUntilExit()
guard pack.terminationStatus == 0 else { fatalError("macOS icon packaging failed.") }
// Check the packed file through the same native decoder used by macOS consumers.
guard let packed = CGImageSourceCreateWithURL(output.appendingPathComponent("app_icon.icns") as CFURL, nil)
else { fatalError("The packaged icon could not be decoded.") }
var decodedSizes: [Int] = []
for index in 0..<CGImageSourceGetCount(packed)
{
    guard let decoded = CGImageSourceCreateImageAtIndex(packed, index, nil), decoded.width == decoded.height
    else { fatalError("The packaged icon contains an invalid representation.") }
    decodedSizes.append(decoded.width)
}
guard decodedSizes.sorted() == [16, 32, 32, 64, 128, 256, 256, 512, 512, 1024]
else { fatalError("The packaged icon is missing a required size.") }
print("Composed the original artwork into a full tile and verified all packaged icon sizes.")
