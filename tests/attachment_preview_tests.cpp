#include "test_harness.h"
#include "app/attachment_preview_service.h"
#include <array>
#if defined(__APPLE__)
#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>
#elif defined(_WIN32)
#include <windows.h>
#include <wincodec.h>
#endif

namespace
{
	void U32(std::string& bytes, std::uint32_t value)
	{
		for (int shift : {24, 16, 8, 0}) bytes += static_cast<char>(value >> shift);
	}
	void Chunk(std::string& bytes, std::string_view type, std::string_view data)
	{
		U32(bytes, static_cast<std::uint32_t>(data.size()));
		const std::size_t start = bytes.size(); bytes += type; bytes += data;
		std::uint32_t crc = 0xffffffffu;
		for (std::size_t index = start; index < bytes.size(); ++index)
		{
			crc ^= static_cast<unsigned char>(bytes[index]);
			for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
		U32(bytes, ~crc);
	}
	unsigned FirstPixelAlpha(std::string_view png)
	{
#if defined(__APPLE__)
		CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(png.data()), png.size());
		CGImageSourceRef decoder = CGImageSourceCreateWithData(data, nullptr); CFRelease(data);
		CGImageRef image = decoder ? CGImageSourceCreateImageAtIndex(decoder, 0, nullptr) : nullptr;
		if (decoder) CFRelease(decoder);
		if (!image) return 999;
		unsigned char pixel[4]{};
		CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
		CGContextRef context = CGBitmapContextCreate(pixel, 1, 1, 8, 4, colors, static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
		CGColorSpaceRelease(colors);
		if (context) { CGContextDrawImage(context, CGRectMake(0, 0, 1, 1), image); CGContextRelease(context); }
		CGImageRelease(image); return pixel[3];
#else
		const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		IWICImagingFactory* factory = nullptr; IWICStream* stream = nullptr; IWICBitmapDecoder* decoder = nullptr;
		IWICBitmapFrameDecode* frame = nullptr; IWICFormatConverter* converter = nullptr;
		unsigned char pixel[4]{}; WICRect rect{0, 0, 1, 1};
		const bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
		    SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), static_cast<DWORD>(png.size()))) &&
		    SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) && SUCCEEDED(decoder->GetFrame(0, &frame)) &&
		    SUCCEEDED(factory->CreateFormatConverter(&converter)) && SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
		    SUCCEEDED(converter->CopyPixels(&rect, 4, 4, pixel));
		if (converter) converter->Release(); if (frame) frame->Release(); if (decoder) decoder->Release();
		if (stream) stream->Release(); if (factory) factory->Release(); if (SUCCEEDED(init)) CoUninitialize();
		return ok ? pixel[3] : 999;
#endif
	}
	/// A genuine RGBA PNG, using stored DEFLATE blocks so tests need no image/zlib dependency.
	std::string Png(unsigned width, unsigned height, bool noisy = false)
	{
		std::string raw;
		std::uint32_t random = 0x12345678;
		for (unsigned row = 0; row < height; ++row)
		{
			raw += '\0';
			for (unsigned column = 0; column < width; ++column)
				for (int channel = 0; channel < 4; ++channel)
				{
					random ^= random << 13; random ^= random >> 17; random ^= random << 5;
					raw += static_cast<char>(noisy ? random : channel == 3 ? 128 : channel == 0 ? 255 : 0);
				}
		}
		std::string compressed("\x78\x01", 2);
		for (std::size_t offset = 0; offset < raw.size();)
		{
			const std::size_t count = std::min<std::size_t>(65535, raw.size() - offset);
			compressed += offset + count == raw.size() ? '\x01' : '\0';
			compressed += static_cast<char>(count); compressed += static_cast<char>(count >> 8);
			compressed += static_cast<char>(~count); compressed += static_cast<char>((~count) >> 8);
			compressed.append(raw, offset, count); offset += count;
		}
		std::uint32_t a = 1, b = 0;
		for (unsigned char value : raw) { a = (a + value) % 65521; b = (b + a) % 65521; }
		U32(compressed, b << 16 | a);
		std::string result("\x89PNG\r\n\x1a\n", 8), header;
		U32(header, width); U32(header, height); header.append("\x08\x06\0\0\0", 5);
		Chunk(result, "IHDR", header); Chunk(result, "IDAT", compressed); Chunk(result, "IEND", {});
		return result;
	}
}

UAM_TEST(AttachmentPreviewDecodesRealPngAndBoundsNoisyOutput)
{
	const uam::attachment_preview::Image decoded = uam::attachment_preview::Decode(Png(2, 3));
	UAM_ASSERT(!decoded.png.empty()); UAM_ASSERT_EQ(decoded.width, 2u); UAM_ASSERT_EQ(decoded.height, 3u);
	UAM_ASSERT_EQ(FirstPixelAlpha(decoded.png), 128u);
	const uam::attachment_preview::Image noisy = uam::attachment_preview::Decode(Png(1024, 1024, true));
	UAM_ASSERT(!noisy.png.empty());
	UAM_ASSERT(noisy.png.size() <= uam::attachment_preview::kMaxPreviewBytes);
	UAM_ASSERT(noisy.width <= 1024 && noisy.height <= 1024);
	const uam::attachment_preview::Image again = uam::attachment_preview::Decode(noisy.png);
	UAM_ASSERT_EQ(again.width, noisy.width); UAM_ASSERT_EQ(again.height, noisy.height);
	std::string jpeg;
	UAM_ASSERT(uam::base64::Decode("/9j/4AAQSkZJRgABAQAAAQABAAD/4QAiRXhpZgAATU0AKgAAAAgAAQESAAMAAAABAAYAAAAAAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAACAAMDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDxKiiivyo/0jP/2Q==", jpeg));
	const uam::attachment_preview::Image oriented = uam::attachment_preview::Decode(jpeg);
	UAM_ASSERT(!oriented.png.empty()); UAM_ASSERT_EQ(oriented.width, 2u); UAM_ASSERT_EQ(oriented.height, 3u);
	UAM_ASSERT(uam::attachment_preview::Decode("not an image").png.empty());
	UAM_ASSERT(uam::attachment_preview::Decode(std::string(uam::attachment_preview::kMaxSourceBytes + 1, 'x')).png.empty());
	// Huge declared dimensions are refused before pixel decoding, even with a tiny corrupt body.
	std::string huge("\x89PNG\r\n\x1a\n", 8), header;
	U32(header, 16385); U32(header, 1); header.append("\x08\x06\0\0\0", 5); Chunk(huge, "IHDR", header); Chunk(huge, "IEND", {});
	UAM_ASSERT(uam::attachment_preview::Decode(huge).png.empty());
	header.clear(); U32(header, 6000); U32(header, 5000); header.append("\x08\x06\0\0\0", 5);
	huge.assign("\x89PNG\r\n\x1a\n", 8); Chunk(huge, "IHDR", header); Chunk(huge, "IEND", {});
	UAM_ASSERT(uam::attachment_preview::Decode(huge).png.empty());
}

UAM_TEST(AttachmentPreviewImmutableCacheSurvivesSourceRemovalAndRejectsTampering)
{
	uam_test::TempDir temp("attachment-preview");
	MessageAttachment attachment;
	attachment.mime_type = "image/png";
	attachment.path = ".UAM/attachments/original.png";
	attachment.preview_key = uam::attachment_preview::Store(temp.root, Png(8, 7));
	UAM_ASSERT(uam::attachment_preview::IsKey(attachment.preview_key));
	UAM_ASSERT(!uam::attachment_preview::Read(temp.root, attachment, false).png.empty());
	const std::filesystem::path cache = temp.root / "attachment-previews" / (attachment.preview_key + ".png");
	UAM_ASSERT(uam::io::WriteBinaryFile(cache, "changed"));
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, true).png.empty());
	attachment.preview_key = "../outside";
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, true).png.empty());
}

UAM_TEST(AttachmentPreviewLegacyUsesOnlyRecordedAbsoluteLocalOrigin)
{
	uam_test::TempDir temp("attachment-preview");
	const std::filesystem::path source = temp.root / "legacy.png";
	UAM_ASSERT(uam::io::WriteBinaryFile(source, Png(4, 5)));
	MessageAttachment attachment;
	attachment.mime_type = "image/png";
	attachment.path = uam::paths::Utf8PathString(source);
	UAM_ASSERT(!uam::attachment_preview::Read(temp.root, attachment, true).png.empty());
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false).png.empty());
	attachment.path = "legacy.png";
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, true).png.empty());
	attachment.preview_origin_path = uam::paths::Utf8PathString(source);
	UAM_ASSERT(!uam::attachment_preview::Read(temp.root, attachment, true).png.empty());
	const std::string absolute_source = uam::paths::Utf8PathString(source);
	attachment.preview_origin_path = absolute_source + std::string(1, '\0') + "ignored.png";
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, true).png.empty());
	attachment.preview_origin_path.clear();
	attachment.path = absolute_source + std::string(1, '\0') + "ignored.png";
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, true).png.empty());
	attachment.path.clear();
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, true).png.empty());
#if !defined(_WIN32)
	const std::filesystem::path link = temp.root / "link.png";
	std::filesystem::create_symlink(source, link);
	attachment.preview_origin_path = uam::paths::Utf8PathString(link);
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, true).png.empty());
#endif
}

UAM_TEST(AttachmentPreviewMetadataRoundTripsMessagesAndQueuedPrompts)
{
	uam_test::TempDir temp("attachment-preview");
	ChatSession chat; chat.id = "preview-persistence"; chat.title = "Preview";
	MessageAttachment attachment;
	attachment.id = "image-one"; attachment.name = "image.png"; attachment.path = ".UAM/attachments/image.png";
	attachment.mime_type = "image/png"; attachment.preview_key = std::string(64, 'a'); attachment.preview_origin_path = "/captured/original.png";
	Message message; message.role = MessageRole::User; message.attachments.push_back(attachment); chat.messages.push_back(message);
	uam::AcpQueuedUserPromptState queued; queued.id = "queued"; queued.attachments.push_back(attachment);
	chat.acp_queued_prompts.push_back(queued);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id, true);
	UAM_ASSERT(loaded.has_value()); UAM_ASSERT_EQ(loaded->messages[0].attachments[0], attachment);
	UAM_ASSERT_EQ(loaded->acp_queued_prompts[0].attachments[0], attachment);
}

#if defined(_WIN32)
UAM_TEST(AttachmentPreviewWindowsNormalizesActualLongCacheAndOriginPaths)
{
	uam_test::TempDir temp("attachment-preview-long-path");
	std::filesystem::path data_root = temp.root;
	while (data_root.native().size() < 280) data_root /= std::wstring(60, L'd');
	const std::filesystem::path ordinary_cache_root = data_root / "attachment-previews";
	UAM_ASSERT(ordinary_cache_root.native().size() >= 260);
	std::filesystem::path native_root;
	std::string normalization_error;
	UAM_ASSERT(uam::io::NativeFilePath(data_root, native_root, normalization_error));
	// Walk the normalized descendant back to the same exclusively owned TempDir root.
	// Retain its extended prefix so assertion failures also reclaim long descendants.
	std::filesystem::path native_cleanup_root = native_root;
	const std::filesystem::path relative_descendant = data_root.lexically_relative(temp.root);
	for (const std::filesystem::path& component : relative_descendant)
	{
		UAM_ASSERT(component != ".." && component != ".");
		native_cleanup_root = native_cleanup_root.parent_path();
	}
	struct NativeOwnedRootCleanup
	{
		std::filesystem::path root;
		~NativeOwnedRootCleanup()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup{native_cleanup_root};
	std::error_code directory_error;
	UAM_ASSERT(std::filesystem::create_directories(native_root, directory_error));
	UAM_ASSERT(!directory_error);
	MessageAttachment cached;
	cached.mime_type = "image/png";
	cached.path = ".UAM/attachments/remote-image.png";
	cached.preview_key = uam::attachment_preview::Store(data_root, Png(8, 7));
	UAM_ASSERT(uam::attachment_preview::IsKey(cached.preview_key));
	const uam::attachment_preview::Image restored = uam::attachment_preview::Read(data_root, cached, false);
	UAM_ASSERT(!restored.png.empty()); UAM_ASSERT_EQ(restored.width, 8u); UAM_ASSERT_EQ(restored.height, 7u);
	const std::filesystem::path ordinary_origin = data_root / (std::wstring(90, L'o') + L".png");
	UAM_ASSERT(ordinary_origin.native().size() >= 260);
	std::filesystem::path native_origin;
	UAM_ASSERT(uam::io::NativeFilePath(ordinary_origin, native_origin, normalization_error));
	UAM_ASSERT(uam::io::WriteBinaryFile(native_origin, Png(4, 5)));
	MessageAttachment legacy;
	legacy.mime_type = "image/png";
	legacy.path = ".UAM/attachments/recorded-image.png";
	legacy.preview_origin_path = uam::paths::Utf8PathString(ordinary_origin);
	const uam::attachment_preview::Image local = uam::attachment_preview::Read(temp.root, legacy, true);
	UAM_ASSERT(!local.png.empty()); UAM_ASSERT_EQ(local.width, 4u); UAM_ASSERT_EQ(local.height, 5u);
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, legacy, false).png.empty());
	std::error_code cleanup_error;
	std::filesystem::remove_all(native_root, cleanup_error);
	UAM_ASSERT(!cleanup_error);
	UAM_ASSERT(!std::filesystem::exists(native_root));
}
#endif
