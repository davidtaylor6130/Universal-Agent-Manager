import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

// macOS Tahoe places icons that miss Apple's grid on a light legacy plate, so the artwork is clipped to the
// standard 824/1024 rounded tile, inset with a transparent margin and a soft drop shadow, and scaled to fill
// (cropping overflow) rather than letterboxed.
func writeIcon(_ image: CGImage, size: Int, destination: URL) throws
{
    let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    guard let context = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8,
        bytesPerRow: size * 4, space: colorSpace, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    else { throw NSError(domain: "UAMIcon", code: 1) }
    let side = CGFloat(size)
    let body = side * 824 / 1024
    let inset = (side - body) / 2
    let tileRect = CGRect(x: inset, y: inset + side * 10 / 1024, width: body, height: body)
    let tile = CGPath(roundedRect: tileRect, cornerWidth: body * 0.2237, cornerHeight: body * 0.2237, transform: nil)
    let fill = CGColor(colorSpace: colorSpace, components: [24.0 / 255, 24.0 / 255, 26.0 / 255, 1])!
    context.saveGState()
    context.setShadow(offset: CGSize(width: 0, height: -side * 10 / 1024), blur: side * 28 / 1024,
        color: CGColor(colorSpace: colorSpace, components: [0, 0, 0, 0.45])!)
    context.addPath(tile)
    context.setFillColor(fill)
    context.fillPath()
    context.restoreGState()
    context.saveGState()
    context.addPath(tile)
    context.clip()
    let scale = body / CGFloat(min(image.width, image.height))
    let width = CGFloat(image.width) * scale
    let height = CGFloat(image.height) * scale
    context.interpolationQuality = .high
    context.draw(image, in: CGRect(x: tileRect.midX - width / 2, y: tileRect.midY - height / 2, width: width, height: height))
    context.restoreGState()
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
print("Composed the original artwork onto the macOS icon grid and verified all packaged icon sizes.")
