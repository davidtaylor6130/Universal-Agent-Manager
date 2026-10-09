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

	Image Decode(std::string_view source)
	{
		if (source.empty() || source.size() > kMaxSourceBytes) return {};
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
		const bool supported = type && (CFEqual(type, CFSTR("public.png")) || CFEqual(type, CFSTR("public.jpeg")));
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
		Image result;
		for (int side : {1024, 512, 256})
		{
			CFNumberRef size = CFNumberCreate(nullptr, kCFNumberIntType, &side);
			const void* keys[] = {kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceCreateThumbnailWithTransform,
			    kCGImageSourceThumbnailMaxPixelSize, kCGImageSourceShouldCache};
			const void* values[] = {kCFBooleanTrue, kCFBooleanTrue, size, kCFBooleanFalse};
			CFDictionaryRef settings = CFDictionaryCreate(nullptr, keys, values, 4,
			    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CGImageRef image = CGImageSourceCreateThumbnailAtIndex(decoder, 0, settings);
			CFRelease(settings);
			CFRelease(size);
			if (!image) break;
			if (CGImageGetWidth(image) == 0 || CGImageGetHeight(image) == 0 || CGImageGetWidth(image) > kMaxPreviewSide || CGImageGetHeight(image) > kMaxPreviewSide)
			{
				CGImageRelease(image);
				break;
			}
			struct Output { std::string bytes; bool overflow = false; } output;
			CGDataConsumerCallbacks callbacks{};
			callbacks.putBytes = [](void* opaque, const void* bytes, std::size_t count) -> std::size_t
			{
				Output* out = static_cast<Output*>(opaque);
				if (out->overflow || count > kMaxPreviewBytes - out->bytes.size()) { out->overflow = true; return 0; }
				out->bytes.append(static_cast<const char*>(bytes), count);
				return count;
			};
			CGDataConsumerRef consumer = CGDataConsumerCreate(&output, &callbacks);
			CGImageDestinationRef encoder = consumer ? CGImageDestinationCreateWithDataConsumer(consumer, CFSTR("public.png"), 1, nullptr) : nullptr;
			if (encoder)
			{
				CGImageDestinationAddImage(encoder, image, nullptr);
				if (CGImageDestinationFinalize(encoder) && !output.overflow && !output.bytes.empty())
					result = {std::move(output.bytes), static_cast<unsigned>(CGImageGetWidth(image)), static_cast<unsigned>(CGImageGetHeight(image))};
				CFRelease(encoder);
			}
			if (consumer) CFRelease(consumer);
			CGImageRelease(image);
			if (!result.png.empty()) break;
		}
		CFRelease(decoder);
		return result;
	}
}
