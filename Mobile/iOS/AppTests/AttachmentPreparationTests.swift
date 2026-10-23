import XCTest
import ImageIO
import UniformTypeIdentifiers
@testable import UAM

final class AttachmentPreparationTests: XCTestCase
{
    func testRealHEICPreservesOriginalAndCreatesOrientedJPEG() throws
    {
        let bytes = try encodedImage(width: 120, height: 80, orientation: 6, type: .heic)
        // Deliberately misleading metadata: conversion follows the bytes, not these labels.
        let input = ComposerAttachment(name: "original.dat", mimeType: "application/octet-stream", data: bytes)
        let pair = try AttachmentPreparation.prepare(input)
        XCTAssertEqual(pair.count, 2)
        XCTAssertEqual(pair[0].id, input.id)
        XCTAssertEqual(pair[0].name, "original.dat")
        XCTAssertEqual(pair[0].mimeType, "application/octet-stream")
        XCTAssertFalse(pair[0].isImage)
        XCTAssertEqual(pair[0].data, bytes)
        XCTAssertEqual(pair[1].name, "original-compatible.jpg")
        XCTAssertEqual(pair[1].mimeType, "image/jpeg")
        XCTAssertLessThanOrEqual(pair[1].data.count, AttachmentPreparation.maximumJPEGBytes)
        let jpeg = try XCTUnwrap(CGImageSourceCreateWithData(pair[1].data as CFData, nil))
        XCTAssertEqual(CGImageSourceGetType(jpeg) as String?, UTType.jpeg.identifier)
        let image = try XCTUnwrap(CGImageSourceCreateImageAtIndex(jpeg, 0, nil))
        XCTAssertEqual(image.width, 80)
        XCTAssertEqual(image.height, 120)
        let properties = try XCTUnwrap(CGImageSourceCopyPropertiesAtIndex(jpeg, 0, nil) as? [CFString: Any])
        XCTAssertNil(properties[kCGImagePropertyGPSDictionary])
        let orientation = (properties[kCGImagePropertyOrientation] as? NSNumber)?.intValue ?? 1
        XCTAssertEqual(orientation, 1)
    }

    func testHEICThumbnailDimensionsAreBounded() throws
    {
        let bytes = try encodedImage(width: 5000, height: 40, orientation: 1, type: .heic)
        let input = ComposerAttachment(name: "wide.heic", mimeType: "image/heic", data: bytes)
        let pair = try AttachmentPreparation.prepare(input)
        let jpeg = try XCTUnwrap(CGImageSourceCreateWithData(pair[1].data as CFData, nil))
        let image = try XCTUnwrap(CGImageSourceCreateImageAtIndex(jpeg, 0, nil))
        XCTAssertEqual(image.width, AttachmentPreparation.maximumPixelDimension)
        XCTAssertLessThanOrEqual(image.height, AttachmentPreparation.maximumPixelDimension)
        XCTAssertEqual(pair[0].data, bytes)
    }

    func testSupportedImagesKeepExactBytesAndIdentity() throws
    {
        for type in [UTType.png, .jpeg, .gif]
        {
            let bytes = try encodedImage(width: 4, height: 3, orientation: 1, type: type)
            let input = ComposerAttachment(name: "picture.\(type.preferredFilenameExtension!)", mimeType: type.preferredMIMEType!, data: bytes)
            XCTAssertEqual(try AttachmentPreparation.prepare(input), [input])
        }
        let webp = try XCTUnwrap(Data(base64Encoded: "UklGRiIAAABXRUJQVlA4IBYAAAAwAQCdASoBAAEADsD+JaQAA3AAAAAA"))
        let input = ComposerAttachment(name: "picture.webp", mimeType: "image/webp", data: webp)
        XCTAssertEqual(try AttachmentPreparation.prepare(input), [input])
    }

    func testMalformedAndOversizedHEICAreRecoverableErrors() throws
    {
        let malformed = Data([0, 0, 0, 24, 0x66, 0x74, 0x79, 0x70, 0x68, 0x65, 0x69, 0x63] + Array(repeating: 0, count: 20))
        let invalid = ComposerAttachment(name: "broken.heic", mimeType: "image/heic", data: malformed)
        XCTAssertThrowsError(try AttachmentPreparation.prepare(invalid))
        let oversized = ComposerAttachment(name: "large.heic", mimeType: "image/heic", data: Data(count: AttachmentPreparation.maximumBytes + 1))
        XCTAssertThrowsError(try AttachmentPreparation.prepare(oversized))
        let document = ComposerAttachment(name: "document.heic", mimeType: "text/plain", data: Data("ordinary document".utf8))
        XCTAssertEqual(try AttachmentPreparation.prepare(document), [document])
    }

    func testPairedImportHonorsCountAndByteLimitsAtomically() throws
    {
        let bytes = try encodedImage(width: 16, height: 12, orientation: 1, type: .heic)
        let original = ComposerAttachment(name: "photo.heic", mimeType: "image/heic", data: bytes)
        let pair = try AttachmentPreparation.prepare(original)
        let small = ComposerAttachment(name: "note.txt", mimeType: "text/plain", data: Data([1]))
        let six = Array(repeating: small, count: 6)
        XCTAssertEqual(try AttachmentPreparation.appending(pair, to: six).count, 8)
        let seven = Array(repeating: small, count: 7)
        XCTAssertThrowsError(try AttachmentPreparation.appending(pair, to: seven))
        XCTAssertEqual(seven.count, 7)
        let pairSize = pair.reduce(0) { $0 + $1.data.count }
        let fillsBudget = ComposerAttachment(name: "existing.dat", mimeType: "application/octet-stream",
            data: Data(count: AttachmentPreparation.maximumBytes - pairSize))
        XCTAssertEqual(try AttachmentPreparation.appending(pair, to: [fillsBudget]).count, 3)
        let exceedsBudget = ComposerAttachment(name: "existing.dat", mimeType: "application/octet-stream",
            data: Data(count: AttachmentPreparation.maximumBytes - pairSize + 1))
        XCTAssertThrowsError(try AttachmentPreparation.appending(pair, to: [exceedsBudget]))
        XCTAssertEqual(exceedsBudget.data.count, AttachmentPreparation.maximumBytes - pairSize + 1)
    }

    func testFileImportReadsExactBytesBeforeHEICPreparation() throws
    {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: folder) }
        let url = folder.appendingPathComponent("original.dat")
        let bytes = try encodedImage(width: 24, height: 16, orientation: 1, type: .heic)
        try bytes.write(to: url)
        let imported = try AttachmentPreparation.readFile(url)
        XCTAssertEqual(imported.name, "original.dat")
        XCTAssertEqual(imported.data, bytes)
        let prepared = try AttachmentPreparation.prepare(imported)
        XCTAssertEqual(prepared.count, 2)
        XCTAssertEqual(prepared[0].data, bytes)
        XCTAssertThrowsError(try AttachmentPreparation.readFile(folder))
    }

    private func encodedImage(width: Int, height: Int, orientation: Int, type: UTType) throws -> Data
    {
        let context = try XCTUnwrap(CGContext(data: nil, width: width, height: height,
            bitsPerComponent: 8, bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
        context.setFillColor(CGColor(red: 0.2, green: 0.5, blue: 0.8, alpha: 1))
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        let image = try XCTUnwrap(context.makeImage())
        let data = NSMutableData()
        let destination = try XCTUnwrap(CGImageDestinationCreateWithData(data, type.identifier as CFString, 1, nil),
            "The test platform must provide ImageIO \(type.identifier) encoding; a missing codec is a failure, not a skipped photo check.")
        CGImageDestinationAddImage(destination, image, [
            kCGImagePropertyOrientation: orientation,
            kCGImagePropertyGPSDictionary: [kCGImagePropertyGPSLatitude: 50, kCGImagePropertyGPSLatitudeRef: "N"],
            kCGImageDestinationLossyCompressionQuality: 0.9] as CFDictionary)
        XCTAssertTrue(CGImageDestinationFinalize(destination))
        return data as Data
    }
}
