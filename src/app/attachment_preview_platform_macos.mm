#include "app/attachment_preview_service.h"
#include <CommonCrypto/CommonDigest.h>
#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <array>

namespace uam::attachment_preview
{
	std::string Digest(std::string_view bytes)
	{
		unsigned char digest[CC_SHA256_DIGEST_LENGTH]{};
		CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest);
		const char* hex = "0123456789abcdef";
		std::string result;
		result.reserve(64);
		for (unsigned char value : digest) { result += hex[value >> 4]; result += hex[value & 15]; }
		return result;
	}

	static DeliveryImage ProcessImage(std::string_view source, std::size_t max_bytes, bool delivery, std::stop_token cancel)
	{
		if (cancel.stop_requested() || source.empty() || source.size() > kMaxSourceBytes || max_bytes == 0 || max_bytes > kMaxSourceBytes) return {};
		const std::string mime = ImageMime(source);
		if (mime.empty()) return {};
		CFDataRef data = CFDataCreateWithBytesNoCopy(nullptr, reinterpret_cast<const UInt8*>(source.data()),
		    static_cast<CFIndex>(source.size()), kCFAllocatorNull);
		if (!data) return {};
		const void* option_keys[] = {kCGImageSourceShouldCache};
		const void* option_values[] = {kCFBooleanFalse};
		CFDictionaryRef options = CFDictionaryCreate(nullptr, option_keys, option_values, 1,
		    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CGImageSourceRef decoder = CGImageSourceCreateWithData(data, options);
		CFRelease(data);
		CFRelease(options);
		if (!decoder) return {};
		const CFStringRef type = CGImageSourceGetType(decoder);
		const bool supported = type && !mime.empty() && (delivery || CFEqual(type, CFSTR("public.png")) || CFEqual(type, CFSTR("public.jpeg")));
		CFDictionaryRef properties = supported ? CGImageSourceCopyPropertiesAtIndex(decoder, 0, nullptr) : nullptr;
		std::int64_t width = 0, height = 0;
		if (properties)
		{
			CFTypeRef w = CFDictionaryGetValue(properties, kCGImagePropertyPixelWidth);
			CFTypeRef h = CFDictionaryGetValue(properties, kCGImagePropertyPixelHeight);
			if (w && h && CFGetTypeID(w) == CFNumberGetTypeID() && CFGetTypeID(h) == CFNumberGetTypeID())
			{
				CFNumberGetValue(static_cast<CFNumberRef>(w), kCFNumberSInt64Type, &width);
				CFNumberGetValue(static_cast<CFNumberRef>(h), kCFNumberSInt64Type, &height);
			}
			CFRelease(properties);
		}
		if (width <= 0 || height <= 0 || width > kMaxSourceSide || height > kMaxSourceSide || width * height > kMaxSourcePixels)
		{
			CFRelease(decoder);
			return {};
		}
		const bool animated = CGImageSourceGetCount(decoder) > 1;
		const bool original_fits = delivery && source.size() <= max_bytes && width <= 8000 && height <= 8000;
		if (delivery && !original_fits && animated) { CFRelease(decoder); return {}; }
		DeliveryImage result;
		const std::vector<int> sides = delivery ? std::vector<int>{8000, 4096, 2576, 2048, 1568, 1024, 768, 512, 256} : std::vector<int>{1024, 512, 256};
		for (int side : sides)
		{
			if (cancel.stop_requested()) { CFRelease(decoder); return {}; }
			const int decode_side = original_fits ? 1024 : side;
			CFNumberRef size = CFNumberCreate(nullptr, kCFNumberIntType, &decode_side);
			const void* keys[] = {kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceCreateThumbnailWithTransform,
			    kCGImageSourceThumbnailMaxPixelSize, kCGImageSourceShouldCache};
			const void* values[] = {kCFBooleanTrue, kCFBooleanTrue, size, kCFBooleanFalse};
			CFDictionaryRef settings = CFDictionaryCreate(nullptr, keys, values, 4,
			    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CGImageRef image = CGImageSourceCreateThumbnailAtIndex(decoder, 0, settings);
			CFRelease(settings);
			CFRelease(size);
			if (!image) break;
			if (cancel.stop_requested()) { CGImageRelease(image); CFRelease(decoder); return {}; }
			if (CGImageGetWidth(image) == 0 || CGImageGetHeight(image) == 0 || CGImageGetWidth(image) > static_cast<unsigned>(side) || CGImageGetHeight(image) > static_cast<unsigned>(side))
			{
				CGImageRelease(image);
				break;
			}
			if (original_fits)
			{
				// Materialize the bounded thumbnail so lazy decoding cannot admit a corrupt body.
				CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
				CGContextRef bitmap = CGBitmapContextCreate(nullptr, CGImageGetWidth(image), CGImageGetHeight(image), 8,
				    CGImageGetWidth(image) * 4, colors, static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
				CGColorSpaceRelease(colors);
				if (!bitmap) { CGImageRelease(image); CFRelease(decoder); return {}; }
				CGContextDrawImage(bitmap, CGRectMake(0, 0, CGImageGetWidth(image), CGImageGetHeight(image)), image);
				CGContextRelease(bitmap);
				CGImageRelease(image); CFRelease(decoder);
				if (cancel.stop_requested()) return {};
				return {std::string(source), static_cast<unsigned>(width), static_cast<unsigned>(height), mime, animated};
			}
			struct Output { std::string bytes; std::size_t cap; std::stop_token cancel; bool overflow = false; } output{{}, max_bytes, cancel};
			CGDataConsumerCallbacks callbacks{};
			callbacks.putBytes = [](void* opaque, const void* bytes, std::size_t count) -> std::size_t
			{
				Output* out = static_cast<Output*>(opaque);
				if (out->cancel.stop_requested() || out->overflow || count > out->cap - out->bytes.size()) { out->overflow = true; return 0; }
				out->bytes.append(static_cast<const char*>(bytes), count);
				return count;
			};
			CGDataConsumerRef consumer = CGDataConsumerCreate(&output, &callbacks);
			CGImageDestinationRef encoder = consumer ? CGImageDestinationCreateWithDataConsumer(consumer, CFSTR("public.png"), 1, nullptr) : nullptr;
			if (encoder)
			{
				CGImageDestinationAddImage(encoder, image, nullptr);
				if (CGImageDestinationFinalize(encoder) && !output.overflow && !output.bytes.empty())
					result = {std::move(output.bytes), static_cast<unsigned>(CGImageGetWidth(image)), static_cast<unsigned>(CGImageGetHeight(image)), "image/png"};
				CFRelease(encoder);
			}
			if (consumer) CFRelease(consumer);
			CGImageRelease(image);
			if (!result.bytes.empty()) break;
		}
		CFRelease(decoder);
		return result;
	}
	Image Decode(std::string_view source)
	{
		DeliveryImage decoded = ProcessImage(source, kMaxPreviewBytes, false, {});
		return {std::move(decoded.bytes), decoded.width, decoded.height};
	}
	DeliveryImage PrepareDeliveryImage(std::string_view source, std::size_t max_bytes, std::stop_token cancel)
	{
		return ProcessImage(source, max_bytes, true, cancel);
	}

}
