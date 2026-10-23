import Foundation
import ImageIO
import UniformTypeIdentifiers

/// Preserves HEIF originals while giving every provider a conventional image to inspect.
/// Run preparation on a worker task; only the returned value is published into the draft.
enum AttachmentPreparation
{
    static let maximumCount = 8
    static let maximumBytes = 20 * 1024 * 1024
    static let maximumJPEGBytes = 4 * 1024 * 1024
    static let maximumPixelDimension = 4096
    static let preservationNotice = "Original photo kept as a file; JPEG added for agents."

    static func readFile(_ url: URL) throws -> ComposerAttachment
    {
        try Task.checkCancellation()
        let access = url.startAccessingSecurityScopedResource()
        defer { if access { url.stopAccessingSecurityScopedResource() } }
        let resource = try url.resourceValues(forKeys: [.fileSizeKey, .contentTypeKey, .isRegularFileKey])
        guard resource.isRegularFile == true, (resource.fileSize ?? Int.max) <= maximumBytes else
        { throw PreparationError.messageLimit }
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var data = Data()
        // Bound the actual read too: a file can change after its metadata was inspected.
        while data.count <= maximumBytes
        {
            try Task.checkCancellation()
            guard let chunk = try handle.read(upToCount: min(1024 * 1024, maximumBytes + 1 - data.count)), !chunk.isEmpty else { break }
            data.append(chunk)
        }
        guard data.count <= maximumBytes else { throw PreparationError.messageLimit }
        return ComposerAttachment(name: url.lastPathComponent,
            mimeType: resource.contentType?.preferredMIMEType ?? "application/octet-stream", data: data)
    }

    static func prepare(_ attachment: ComposerAttachment) throws -> [ComposerAttachment]
    {
        try Task.checkCancellation()
        guard attachment.data.count <= maximumBytes else { throw PreparationError.messageLimit }

        // HEIF is an ISO base media file. Avoid sending ordinary documents through ImageIO's
        // codec registry merely because their extension or caller-supplied MIME says image.
        let header = attachment.data.prefix(12)
        let baseMediaFile = header.count == 12 && header.dropFirst(4).prefix(4).elementsEqual([0x66, 0x74, 0x79, 0x70])
        let declaredHEIF = ["image/heic", "image/heif", "image/heic-sequence", "image/heif-sequence"].contains(attachment.mimeType.lowercased())
        guard baseMediaFile else
        {
            if declaredHEIF { throw PreparationError.invalidPhoto }
            return [attachment]
        }
        guard let source = CGImageSourceCreateWithData(attachment.data as CFData, [kCGImageSourceShouldCache: false] as CFDictionary),
              let identifier = CGImageSourceGetType(source), let type = UTType(identifier as String) else
        {
            if declaredHEIF { throw PreparationError.invalidPhoto }
            return [attachment]
        }
        let isHEIF = type.conforms(to: .heic) || type.conforms(to: .heif) ||
            UTType("public.heif-standard").map { type.conforms(to: $0) } == true
        guard isHEIF else { return [attachment] }
        guard CGImageSourceGetCount(source) == 1 else { throw PreparationError.imageSequence }
        let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any]
        guard let width = (properties?[kCGImagePropertyPixelWidth] as? NSNumber)?.intValue,
              let height = (properties?[kCGImagePropertyPixelHeight] as? NSNumber)?.intValue,
              width > 0, height > 0, width <= 100_000, height <= 100_000,
              Int64(width) * Int64(height) <= 200_000_000 else { throw PreparationError.invalidPhoto }

        var jpeg = try thumbnailJPEG(source, maximumDimension: maximumPixelDimension, quality: 0.85)
        if jpeg.count > maximumJPEGBytes
        {
            jpeg = try thumbnailJPEG(source, maximumDimension: 2048, quality: 0.8)
        }
        guard jpeg.count <= maximumJPEGBytes else { throw PreparationError.invalidPhoto }
        try Task.checkCancellation()
        let stem = (attachment.name as NSString).deletingPathExtension
        let original = ComposerAttachment(id: attachment.id, name: attachment.name,
            mimeType: "application/octet-stream", data: attachment.data)
        let compatible = ComposerAttachment(name: "\(stem.isEmpty ? "Photo" : stem)-compatible.jpg",
            mimeType: "image/jpeg", data: jpeg)
        return [original, compatible]
    }

    /// A pair is accepted together or leaves the draft unchanged.
    static func appending(_ prepared: [ComposerAttachment], to existing: [ComposerAttachment]) throws -> [ComposerAttachment]
    {
        guard existing.count + prepared.count <= maximumCount else { throw PreparationError.messageLimit }
        var bytes = 0
        for attachment in existing + prepared
        {
            guard attachment.data.count <= maximumBytes - bytes else { throw PreparationError.messageLimit }
            bytes += attachment.data.count
        }
        return existing + prepared
    }

    private static func thumbnailJPEG(_ source: CGImageSource, maximumDimension: Int, quality: Double) throws -> Data
    {
        try Task.checkCancellation()
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: maximumDimension,
            kCGImageSourceShouldCacheImmediately: true]
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary),
              image.width > 0, image.height > 0,
              image.width <= maximumDimension, image.height <= maximumDimension else { throw PreparationError.invalidPhoto }
        try Task.checkCancellation()
        let encoded = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(encoded, UTType.jpeg.identifier as CFString, 1, nil) else { throw PreparationError.invalidPhoto }
        // Orientation is applied to pixels above. Original EXIF/GPS metadata stays only in the
        // untouched original file; it is not copied into the provider-compatible derivative.
        CGImageDestinationAddImage(destination, image, [kCGImageDestinationLossyCompressionQuality: quality] as CFDictionary)
        guard CGImageDestinationFinalize(destination), encoded.length > 0 else { throw PreparationError.invalidPhoto }
        return encoded as Data
    }

    enum PreparationError: LocalizedError
    {
        case invalidPhoto, imageSequence, messageLimit
        var errorDescription: String?
        {
            switch self
            {
            case .invalidPhoto: "This HEIC/HEIF photo could not be prepared. Choose another photo or export it as JPEG."
            case .imageSequence: "HEIC/HEIF image sequences are not supported. Choose a still photo or export it as JPEG."
            case .messageLimit: "Attach up to eight files, totalling 20 MB per message. A HEIC/HEIF photo uses two files: its original and a compatible JPEG."
            }
        }
    }
}
