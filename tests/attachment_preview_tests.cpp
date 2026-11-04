#include "test_harness.h"
#include "app/attachment_preview_service.h"
#include "common/chat/chat_repository.h"
#include "common/provider/provider_runtime.h"
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
	const uam::attachment_preview::DeliveryImage oriented_delivery = uam::attachment_preview::PrepareDeliveryImage(jpeg, 200);
	UAM_ASSERT(!oriented_delivery.bytes.empty()); UAM_ASSERT_EQ(oriented_delivery.width, 2u); UAM_ASSERT_EQ(oriented_delivery.height, 3u);
	UAM_ASSERT_EQ(oriented_delivery.mime_type, std::string("image/png"));
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

UAM_TEST(AttachmentPreviewLoadsColdPersistedMessageWithoutMutatingLiveChat)
{
	uam_test::TempDir temp("attachment-preview-cold");
	ChatSession chat; chat.id = "cold-preview-chat"; chat.provider_id = "codex"; chat.native_session_id = "native-session";
	chat.created_at = "2026-10-09T12:00:00.000Z"; chat.updated_at = chat.created_at;
	chat.title = "Cold preview";
	MessageAttachment attachment; attachment.id = "image-id"; attachment.name = "image.png";
	attachment.path = ".UAM/attachments/image.png";
	attachment.mime_type = "image/png"; attachment.preview_key = uam::attachment_preview::Store(temp.root, Png(6, 4));
	UAM_ASSERT(uam::attachment_preview::IsKey(attachment.preview_key));
	Message message; message.role = MessageRole::User; message.content = "attached image"; message.attachments.push_back(attachment);
	chat.messages.push_back(message);
	Message trailing; trailing.role = MessageRole::Assistant; trailing.content = "reply"; chat.messages.push_back(trailing);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	std::optional<ChatSession> cold = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(cold.has_value()); UAM_ASSERT(!cold->messages_loaded); UAM_ASSERT(cold->messages.empty());
	const uam::attachment_preview::ChatSnapshot snapshot = uam::attachment_preview::CaptureChatSnapshot(*cold);
	UAM_ASSERT(!snapshot.messages_digest.empty());
	const std::optional<uam::attachment_preview::AttachmentSnapshot> resolved =
	    uam::attachment_preview::LoadPersistedAttachmentSnapshot(temp.root, snapshot, 0, "image-id");
	UAM_ASSERT(resolved.has_value()); UAM_ASSERT_EQ(resolved->attachment, attachment);
	UAM_ASSERT(!cold->messages_loaded); UAM_ASSERT(cold->messages.empty());
	UAM_ASSERT(!uam::attachment_preview::Read(temp.root, resolved->attachment, true).png.empty());

	UAM_ASSERT(!uam::attachment_preview::LoadPersistedAttachmentSnapshot(temp.root, snapshot, 0, "missing").has_value());
	UAM_ASSERT(!uam::attachment_preview::LoadPersistedAttachmentSnapshot(temp.root, snapshot, 2, "image-id").has_value());
	uam::attachment_preview::ChatSnapshot stale = snapshot; ++stale.message_count;
	UAM_ASSERT(!uam::attachment_preview::LoadPersistedAttachmentSnapshot(temp.root, stale, 0, "image-id").has_value());
	stale = snapshot; stale.execution_host_id = "remote-host";
	UAM_ASSERT(!uam::attachment_preview::LoadPersistedAttachmentSnapshot(temp.root, stale, 0, "image-id").has_value());
	stale = snapshot; stale.native_identity_key += "stale";
	UAM_ASSERT(!uam::attachment_preview::LoadPersistedAttachmentSnapshot(temp.root, stale, 0, "image-id").has_value());
	stale = snapshot; stale.messages_digest += "stale";
	UAM_ASSERT(!uam::attachment_preview::LoadPersistedAttachmentSnapshot(temp.root, stale, 0, "image-id").has_value());

	std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id, true);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(uam::attachment_preview::MatchesCurrentAttachmentSnapshot(*loaded, snapshot, 0, "image-id", *resolved));
	loaded->messages[0].attachments[0].path = "changed while reading";
	UAM_ASSERT(!uam::attachment_preview::MatchesCurrentAttachmentSnapshot(*loaded, snapshot, 0, "image-id", *resolved));
	UAM_ASSERT_EQ(cold->messages.size(), 0u); UAM_ASSERT(!cold->messages_loaded);
	UAM_ASSERT_EQ(loaded->messages[0].content, message.content);
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

UAM_TEST(ClaudeDeliveryPreservesFittingOriginalsAndRejectsInvalidOrAnimatedOverflow)
{
	const std::string original = Png(8, 7);
	const uam::attachment_preview::DeliveryImage unchanged = uam::attachment_preview::PrepareDeliveryImage(original, original.size());
	UAM_ASSERT_EQ(unchanged.bytes, original);
	UAM_ASSERT_EQ(unchanged.mime_type, std::string("image/png"));
	UAM_ASSERT_EQ(FirstPixelAlpha(unchanged.bytes), 128u);
	const std::string noisy = Png(700, 700, true);
	const uam::attachment_preview::DeliveryImage smaller = uam::attachment_preview::PrepareDeliveryImage(noisy, 400 * 1024);
	UAM_ASSERT(!smaller.bytes.empty() && smaller.bytes.size() <= 400 * 1024);
	UAM_ASSERT(smaller.width < 700 && smaller.height < 700);
	UAM_ASSERT(!uam::attachment_preview::Decode(smaller.bytes).png.empty());
	std::string corrupt("\x89PNG\r\n\x1a\n", 8), header;
	U32(header, 2); U32(header, 3); header.append("\x08\x06\0\0\0", 5);
	Chunk(corrupt, "IHDR", header); Chunk(corrupt, "IEND", {});
	UAM_ASSERT(uam::attachment_preview::PrepareDeliveryImage(corrupt, 4096).bytes.empty());
	std::string gif;
	UAM_ASSERT(uam::base64::Decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", gif));
	const std::string frames = gif.substr(19, gif.size() - 20);
	gif.insert(gif.size() - 1, frames);
	UAM_ASSERT_EQ(uam::attachment_preview::PrepareDeliveryImage(gif, gif.size()).bytes, gif);
	UAM_ASSERT(uam::attachment_preview::PrepareDeliveryImage(gif, gif.size() - 1).bytes.empty());
	std::stop_source canceled; canceled.request_stop();
	UAM_ASSERT(uam::attachment_preview::PrepareDeliveryImage(original, original.size(), canceled.get_token()).bytes.empty());
}

UAM_TEST(ClaudeDeliveryBudgetIncludesEscapedPromptAndEveryImageWithoutChangingSources)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	uam_test::TempDir temp("claude-delivery-budget");
	const std::string original = Png(700, 700, true);
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / "one.png", original));
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / "two.png", original));
	MessageAttachment one; one.id = "one"; one.name = "one.png"; one.path = one.name; one.kind = "image"; one.mime_type = "image/jpeg";
	MessageAttachment two = one; two.id = "two"; two.name = "two.png"; two.path = two.name;
	const std::vector<MessageAttachment> images{one, two};
	std::string error;
	const std::string prompt = "Inspect both images.\n\"Quoted\" text and \\ path.";
	const nlohmann::json message = uam::claude_input::Build(prompt, temp.root, images, error, true);
	UAM_ASSERT(error.empty()); UAM_ASSERT(!message.is_null());
	UAM_ASSERT(message.dump().size() + 1 <= uam::platform::kAsyncInputMaxQueuedBytes);
	UAM_ASSERT_EQ(message["message"]["content"][0]["text"], nlohmann::json(prompt));
	UAM_ASSERT_EQ(message["message"]["content"].size(), std::size_t{3});
	for (std::size_t index = 1; index <= 2; ++index)
	{
		const nlohmann::json& source = message["message"]["content"][index]["source"];
		UAM_ASSERT_EQ(source["media_type"], nlohmann::json("image/png"));
		std::string delivered;
		UAM_ASSERT(uam::base64::Decode(source["data"].get<std::string>(), delivered));
		UAM_ASSERT(delivered.size() < original.size());
		UAM_ASSERT(!uam::attachment_preview::Decode(delivered).png.empty());
	}
	std::string after;
	UAM_ASSERT(uam::attachment_preview::ReadDeliverySource(temp.root / "one.png", after)); UAM_ASSERT_EQ(after, original);
	UAM_ASSERT(uam::attachment_preview::ReadDeliverySource(temp.root / "two.png", after)); UAM_ASSERT_EQ(after, original);
	UAM_ASSERT_EQ(images.front(), one); UAM_ASSERT_EQ(images.back(), two);
	UAM_ASSERT(uam::claude_input::Build(std::string(uam::platform::kAsyncInputMaxQueuedBytes, '"'), temp.root, images, error, true).is_null());
	UAM_ASSERT(error.find("4 MiB") != std::string::npos);
	// Remote runtime input never dereferences a runner's path on this machine.
	ChatSession remote; remote.execution_host_id = "remote"; remote.workspace_directory = temp.root.string();
	Message user; user.role = MessageRole::User; user.attachments = images; remote.messages.push_back(user);
	uam::AcpSessionState session; session.turn_first_user_message_index = 0; session.turn_user_message_index = 0;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("claude-cli"); std::string method;
	one.path = "missing.png"; remote.messages[0].attachments = {one};
	const nlohmann::json remote_message = runtime.OnAcpBuildPrompt(session, 1, prompt, remote, method);
	UAM_ASSERT(session.last_error.empty()); UAM_ASSERT_EQ(remote_message["message"]["content"].size(), std::size_t{1});
#endif
}

UAM_TEST(ClaudeSendPreflightRejectsMissingUnsupportedAndOversizedLocalImages)
{
	uam_test::TempDir temp("claude-image-preflight");
	MessageAttachment image; image.kind = "image"; image.name = "one.png"; image.path = image.name;
	std::string error;
	UAM_ASSERT(!uam::attachment_preview::ValidateImageAttachmentFiles(temp.root, {image}, error));
	UAM_ASSERT(error.find("no longer available") != std::string::npos);
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / image.path, "wrong format"));
	UAM_ASSERT(!uam::attachment_preview::ValidateImageAttachmentFiles(temp.root, {image}, error));
	UAM_ASSERT(error.find("Unsupported") != std::string::npos);
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / image.path, Png(2, 3)));
	UAM_ASSERT(uam::attachment_preview::ValidateImageAttachmentFiles(temp.root, {image}, error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT(!uam::attachment_preview::ValidateImageAttachmentFiles(temp.root, std::vector<MessageAttachment>(33, image), error));
	UAM_ASSERT(error.find("32") != std::string::npos);
	std::filesystem::resize_file(temp.root / image.path, uam::attachment_preview::kMaxSourceBytes + 1);
	UAM_ASSERT(!uam::attachment_preview::ValidateImageAttachmentFiles(temp.root, {image}, error));
	UAM_ASSERT(error.find("25 MiB") != std::string::npos);
#if !defined(_WIN32)
	const std::filesystem::path link = temp.root / "link.png";
	std::filesystem::create_symlink(temp.root / image.path, link);
	image.path = "link.png";
	UAM_ASSERT(!uam::attachment_preview::ValidateImageAttachmentFiles(temp.root, {image}, error));
#endif
}

UAM_TEST(ClaudeDeliveryKeepsTinyAndAnimatedImagesWhileCompressingLargeStaticImage)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	uam_test::TempDir temp("claude-mixed-images");
	const std::string tiny = Png(1, 1);
	const std::string medium = Png(32, 32, true);
	std::string gif; UAM_ASSERT(uam::base64::Decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", gif));
	gif.insert(gif.size() - 1, gif.substr(19, gif.size() - 20));
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / "tiny.png", tiny));
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / "animated.gif", gif));
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / "medium.png", medium));
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / "large.png", Png(1024, 1024, true)));
	std::vector<MessageAttachment> images;
	for (const std::string& name : {"tiny.png", "animated.gif", "medium.png", "large.png"})
	{ MessageAttachment image; image.kind = "image"; image.path = name; image.name = name; images.push_back(image); }
	std::string error;
	const nlohmann::json message = uam::claude_input::Build("Describe every image", temp.root, images, error, true);
	UAM_ASSERT(error.empty()); UAM_ASSERT(!message.is_null());
	UAM_ASSERT(message.dump().size() + 1 <= uam::platform::kAsyncInputMaxQueuedBytes);
	std::string delivered;
	UAM_ASSERT(uam::base64::Decode(message["message"]["content"][1]["source"]["data"].get<std::string>(), delivered)); UAM_ASSERT_EQ(delivered, tiny);
	UAM_ASSERT(uam::base64::Decode(message["message"]["content"][2]["source"]["data"].get<std::string>(), delivered)); UAM_ASSERT_EQ(delivered, gif);
	UAM_ASSERT(uam::base64::Decode(message["message"]["content"][3]["source"]["data"].get<std::string>(), delivered)); UAM_ASSERT_EQ(delivered, medium);
#endif
}

UAM_TEST(AttachmentPreviewIgnoresNonImageBytesWithoutCaching)
{
	uam_test::TempDir temp("attachment-preview-nonimage");
	const std::string original(112 * 1024, 'x');
	const std::filesystem::path original_path = temp.root / "ordinary.png";
	UAM_ASSERT(uam::io::WriteBinaryFile(original_path, original));
	UAM_ASSERT(uam::attachment_preview::Store(temp.root, original).empty());
	UAM_ASSERT(uam::attachment_preview::Decode(original).png.empty());
	UAM_ASSERT(uam::attachment_preview::PrepareDeliveryImage(original, original.size()).bytes.empty());
	UAM_ASSERT(!std::filesystem::exists(temp.root / "attachment-previews"));
	UAM_ASSERT_EQ(uam_test::ReadFile(original_path), original);
	// A rejected ordinary file must not poison later recognized-image staging.
	const std::string image = Png(8, 7);
	const std::string key = uam::attachment_preview::Store(temp.root, image);
	UAM_ASSERT(uam::attachment_preview::IsKey(key));
	const uam::attachment_preview::Image preview = uam::attachment_preview::Decode(uam_test::ReadFile(temp.root / "attachment-previews" / (key + ".png")));
	UAM_ASSERT_EQ(preview.width, 8u);
	UAM_ASSERT_EQ(preview.height, 7u);
	UAM_ASSERT_EQ(FirstPixelAlpha(preview.png), 128u);
	UAM_ASSERT_EQ(uam_test::ReadFile(original_path), original);
}

// Owned-native preview cases are appended so the existing fixture/test prefix remains intact.
#include "common/chat/native_attachment_assets.h"
#include "common/chat/native_media_chat_save.h"

#if defined(__APPLE__) || defined(_WIN32)
namespace
{
	struct PreviewOriginal
	{
		std::string bytes;
		std::string mime;
	};

	std::string PreviewOrientedJpeg()
	{
		std::string bytes;
		UAM_ASSERT(uam::base64::Decode("/9j/4AAQSkZJRgABAQAAAQABAAD/4QAiRXhpZgAATU0AKgAAAAgAAQESAAMAAAABAAYAAAAAAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAACAAMDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDxKiiivyo/0jP/2Q==", bytes));
		return bytes;
	}

	MessageAttachment PreviewPendingOriginal(const PreviewOriginal& original, std::string id, std::string host)
	{
		MessageAttachment attachment;
		attachment.id = std::move(id); attachment.name = "Native original"; attachment.kind = "image";
		attachment.source = AttachmentSource::NativeInlinePending;
		attachment.native_part_key = attachment.id;
		attachment.native_tool_call_id = "native-tool";
		attachment.native_source_field = "toolCalls/native-tool/result/0";
		attachment.origin_execution_host_id = std::move(host);
		std::string error;
		attachment.native_inline_payload = uam::native_assets::MakeInlinePayload(uam::base64::Encode(original.bytes), original.mime,
		    std::make_shared<uam::native_assets::PayloadBudget>(), error);
		UAM_ASSERT(attachment.native_inline_payload && error.empty());
		attachment.mime_type = attachment.native_inline_payload->MimeType();
		attachment.content_digest = attachment.native_inline_payload->Digest();
		attachment.size_bytes = attachment.native_inline_payload->DecodedSize();
		return attachment;
	}

	ChatSession SavePreviewOriginals(const std::filesystem::path& root, const std::vector<PreviewOriginal>& originals,
	    std::string first_id = "preview-part-0")
	{
		ChatSession chat;
		chat.id = "saved-UAM-preview-owner"; chat.native_session_id = "separate-native-session";
		chat.provider_id = "gemini-cli"; chat.execution_host_id = "saved-remote-origin";
		chat.workspace_directory = uam::paths::Utf8PathString(root);
		chat.created_at = "2026-10-16T12:00:00Z"; chat.updated_at = chat.created_at;
		chat.messages.push_back(Message{MessageRole::User, "Saved native media"});
		UAM_ASSERT(ChatRepository::SaveChat(root, chat));
		const auto primary = AppPaths::UamChatFilePath(root, chat.id);
		const auto before = uam_test::ReadFile(primary);
		const auto revision = ChatRepository::ChatStorageRevision(root, chat.id);
		UAM_ASSERT(revision);
		for (std::size_t index = 0; index < originals.size(); ++index)
			chat.messages.front().attachments.push_back(PreviewPendingOriginal(originals[index],
			    index == 0 ? first_id : "preview-part-" + std::to_string(index), chat.execution_host_id));
		std::string error;
		auto save = uam::PreparedNativeMediaChatSave::Prepare(root, chat, *revision, error);
		UAM_ASSERT(save && error.empty());
		UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
		UAM_ASSERT(!save->PrimaryAcknowledged());
		UAM_ASSERT(!save->TakeCommittedChat());
		UAM_ASSERT(save->Publish(error, [] { return true; }));
		UAM_ASSERT(save->PrimaryAcknowledged());
		UAM_ASSERT(save->AcknowledgeOriginals(error));
		const auto committed = save->TakeCommittedChat(); UAM_ASSERT(committed);
		save.reset();
		const auto loaded = ChatRepository::LoadLocalChat(root, chat.id, true);
		UAM_ASSERT(loaded && loaded->messages_loaded);
		UAM_ASSERT_EQ(loaded->messages.front().attachments, committed->messages.front().attachments);
		UAM_ASSERT_EQ(loaded->messages.front().attachments.size(), originals.size());
		for (std::size_t index = 0; index < originals.size(); ++index)
		{
			const auto& attachment = loaded->messages.front().attachments[index];
			UAM_ASSERT_EQ(attachment.source, AttachmentSource::OwnedNativeAsset);
			UAM_ASSERT(!attachment.native_inline_payload && attachment.path.empty());
			UAM_ASSERT(uam::native_assets::IsKeyForOwner(chat.id, attachment.native_asset_key));
			UAM_ASSERT(!uam::native_assets::IsKeyForOwner(chat.native_session_id, attachment.native_asset_key));
			const auto original = uam::native_assets::ReadOwned(root, chat.id, attachment.native_asset_key);
			UAM_ASSERT(original.ok); UAM_ASSERT_EQ(original.bytes, originals[index].bytes);
		}
		return *loaded;
	}

	std::filesystem::path PreviewOriginalFile(const std::filesystem::path& root, const MessageAttachment& attachment)
	{
		std::filesystem::path original;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(root / std::string(uam::native_assets::kStoreDirectory)))
			if (entry.path().filename() == attachment.content_digest + ".bin")
			{
				UAM_ASSERT(original.empty());
				original = uam_test::NativePath(entry.path());
			}
		UAM_ASSERT(!original.empty());
		return original;
	}

	void AssertBoundedOwnedPreview(const uam::attachment_preview::Image& image)
	{
		UAM_ASSERT(!image.png.empty());
		UAM_ASSERT(image.png.size() <= uam::attachment_preview::kMaxPreviewBytes);
		UAM_ASSERT(image.width > 0 && image.width <= uam::attachment_preview::kMaxPreviewSide);
		UAM_ASSERT(image.height > 0 && image.height <= uam::attachment_preview::kMaxPreviewSide);
		UAM_ASSERT_EQ(uam::attachment_preview::ImageMime(image.png), std::string("image/png"));
	}
}

UAM_TEST(AttachmentPreviewOwnedPrimarySaveDecodesPngAndOrientedJpeg)
{
	uam_test::TempDir temp("owned-preview-originals");
	const auto chat = SavePreviewOriginals(temp.root, {{Png(2048, 512), "image/png"}, {PreviewOrientedJpeg(), "image/jpeg"},
	    {Png(1024, 1024, true), "image/png"}});
	const auto& attachments = chat.messages.front().attachments;
	// Remote provenance does not prevent reading originals already owned by this controller.
	const auto png = uam::attachment_preview::Read(temp.root, attachments[0], false, chat.id);
	AssertBoundedOwnedPreview(png); UAM_ASSERT_EQ(png.width, 1024u); UAM_ASSERT_EQ(png.height, 256u);
	const auto jpeg = uam::attachment_preview::Read(temp.root, attachments[1], false, chat.id);
	AssertBoundedOwnedPreview(jpeg); UAM_ASSERT_EQ(jpeg.width, 2u); UAM_ASSERT_EQ(jpeg.height, 3u);
	const auto noisy = uam::attachment_preview::Read(temp.root, attachments[2], false, chat.id);
	AssertBoundedOwnedPreview(noisy);
	UAM_ASSERT(noisy.width < 1024 && noisy.height < 1024);
	// Reading originals does not create derived preview cache entries.
	UAM_ASSERT(!std::filesystem::exists(temp.root / "attachment-previews"));
	for (const auto& attachment : attachments) UAM_ASSERT(attachment.preview_key.empty());
}

UAM_TEST(AttachmentPreviewOwnedRequiresExactSavedOwner)
{
	uam_test::TempDir temp("owned-preview-owner");
	const auto chat = SavePreviewOriginals(temp.root, {{Png(6, 4), "image/png"}});
	auto attachment = chat.messages.front().attachments.front();
	attachment.preview_key = uam::attachment_preview::Store(temp.root, Png(9, 7));
	UAM_ASSERT(uam::attachment_preview::IsKey(attachment.preview_key));
	AssertBoundedOwnedPreview(uam::attachment_preview::Read(temp.root, attachment, false, chat.id));
	// Existing three-argument callers retain legacy behavior but gain no owned-original authority.
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, true).png.empty());
	for (const auto& owner : {std::string{}, std::string("unknown-UAM-owner"), chat.native_session_id})
	{
		UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, true, owner).png.empty());
		UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, owner).png.empty());
	}
	attachment.preview_key.clear();
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, true, chat.native_session_id).png.empty());
	AssertBoundedOwnedPreview(uam::attachment_preview::Read(temp.root, attachment, false, chat.id));
}

UAM_TEST(AttachmentPreviewOwnedCacheRequiresVerifiedOriginal)
{
	uam_test::TempDir temp("owned-preview-original-integrity");
	const std::string bytes = Png(6, 4);
	const auto chat = SavePreviewOriginals(temp.root, {{bytes, "image/png"}});
	auto attachment = chat.messages.front().attachments.front();
	attachment.preview_key = uam::attachment_preview::Store(temp.root, Png(9, 7));
	UAM_ASSERT(uam::attachment_preview::IsKey(attachment.preview_key));
	const auto cached = uam::attachment_preview::Read(temp.root, attachment, false, chat.id);
	AssertBoundedOwnedPreview(cached); UAM_ASSERT_EQ(cached.width, 9u); UAM_ASSERT_EQ(cached.height, 7u);
	const auto original = PreviewOriginalFile(temp.root, attachment);
	std::string changed = bytes; changed.back() ^= 1;
	UAM_ASSERT(uam::io::WriteBinaryFile(original, changed));
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).png.empty());
	UAM_ASSERT(uam::io::WriteBinaryFile(original, bytes));
	AssertBoundedOwnedPreview(uam::attachment_preview::Read(temp.root, attachment, false, chat.id));
	std::filesystem::resize_file(original, uam::attachment_preview::kMaxSourceBytes + 1);
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).png.empty());
	UAM_ASSERT(uam::io::WriteBinaryFile(original, bytes));
	UAM_ASSERT(std::filesystem::remove(original));
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).png.empty());
	// The independent derived cache is still readable through the unchanged legacy route.
	MessageAttachment legacy; legacy.mime_type = "image/png"; legacy.preview_key = attachment.preview_key;
	AssertBoundedOwnedPreview(uam::attachment_preview::Read(temp.root, legacy, false));
}

UAM_TEST(AttachmentPreviewOwnedExplicitCacheNeverFallsBack)
{
	uam_test::TempDir temp("owned-preview-explicit-cache");
	const auto chat = SavePreviewOriginals(temp.root, {{Png(6, 4), "image/png"}});
	auto attachment = chat.messages.front().attachments.front();
	const auto key = uam::attachment_preview::Store(temp.root, Png(9, 7));
	UAM_ASSERT(uam::attachment_preview::IsKey(key));
	attachment.preview_key = key;
	UAM_ASSERT_EQ(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).width, 9u);
	attachment.preview_key = uam::attachment_preview::Store(temp.root, Png(5, 3));
	UAM_ASSERT(uam::attachment_preview::IsKey(attachment.preview_key));
	UAM_ASSERT_EQ(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).width, 5u);
	attachment.preview_key = "../outside";
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).png.empty());
	attachment.preview_key = std::string(64, '0');
	UAM_ASSERT(attachment.preview_key != key);
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).png.empty());
	attachment.preview_key = key;
	const auto cached = temp.root / "attachment-previews" / (key + ".png");
	UAM_ASSERT(uam::io::WriteBinaryFile(cached, Png(8, 2)));
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).png.empty());
	UAM_ASSERT(std::filesystem::remove(cached));
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).png.empty());
	attachment.preview_key.clear();
	const auto original = uam::attachment_preview::Read(temp.root, attachment, false, chat.id);
	AssertBoundedOwnedPreview(original); UAM_ASSERT_EQ(original.width, 6u); UAM_ASSERT_EQ(original.height, 4u);
}

UAM_TEST(AttachmentPreviewProhibitedSourcesRejectReadableCacheAndLocalPaths)
{
	uam_test::TempDir temp("owned-preview-no-source-fallback");
	const auto path = temp.root / "readable-origin.png";
	UAM_ASSERT(uam::io::WriteBinaryFile(path, Png(4, 5)));
	MessageAttachment legacy; legacy.id = "legacy-image"; legacy.name = "image.png"; legacy.mime_type = "image/png";
	legacy.path = uam::paths::Utf8PathString(path); legacy.preview_origin_path = legacy.path;
	legacy.preview_key = uam::attachment_preview::Store(temp.root, Png(9, 7));
	UAM_ASSERT(uam::attachment_preview::IsKey(legacy.preview_key));
	UAM_ASSERT_EQ(uam::attachment_preview::Read(temp.root, legacy, false).width, 9u);
	auto local_only = legacy; local_only.preview_key.clear();
	UAM_ASSERT_EQ(uam::attachment_preview::Read(temp.root, local_only, true).width, 4u);
	for (const auto source : {AttachmentSource::NativeInlinePending, AttachmentSource::ProviderUri,
	         AttachmentSource::LegacyPath, static_cast<AttachmentSource>(999)})
	{
		auto prohibited = legacy; prohibited.source = source;
		prohibited.source_uri = "file://" + legacy.path;
		for (const bool local : {false, true})
		{
			UAM_ASSERT(uam::attachment_preview::Read(temp.root, prohibited, local, "saved-UAM-preview-owner").png.empty());
			auto no_cache = prohibited; no_cache.preview_key.clear();
			UAM_ASSERT(uam::attachment_preview::Read(temp.root, no_cache, local, "saved-UAM-preview-owner").png.empty());
		}
	}
	// Pure pathless native inputs are also unavailable, even when a cache key is injected.
	auto pending = PreviewPendingOriginal({Png(2, 3), "image/png"}, "pending-part", "local");
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, pending, true, "saved-UAM-preview-owner").png.empty());
	pending.preview_key = legacy.preview_key;
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, pending, true, "saved-UAM-preview-owner").png.empty());
	MessageAttachment uri; uri.id = "uri-part"; uri.name = "External image"; uri.mime_type = "image/png";
	uri.source = AttachmentSource::ProviderUri; uri.native_part_key = "v1:opaque-uri";
	for (const auto& reference : {std::string("https://assets.invalid/original.png"), std::string("file://") + legacy.path,
	         legacy.path, std::string("provider:opaque/image")})
	{
		uri.source_uri = reference;
		UAM_ASSERT(uam::attachment_preview::Read(temp.root, uri, true, "saved-UAM-preview-owner").png.empty());
		uri.preview_key = legacy.preview_key;
		UAM_ASSERT(uam::attachment_preview::Read(temp.root, uri, true, "saved-UAM-preview-owner").png.empty());
		uri.preview_key.clear();
	}
	UAM_ASSERT(!std::filesystem::exists(temp.root / std::string(uam::native_assets::kStoreDirectory)));
	UAM_ASSERT_EQ(uam_test::ReadFile(path), Png(4, 5));
}

UAM_TEST(AttachmentPreviewOwnedRejectsMixedAndInconsistentMetadata)
{
	uam_test::TempDir temp("owned-preview-metadata");
	const auto chat = SavePreviewOriginals(temp.root, {{Png(6, 4), "image/png"}});
	auto attachment = chat.messages.front().attachments.front();
	attachment.preview_key = uam::attachment_preview::Store(temp.root, Png(9, 7));
	const auto readable = temp.root / "readable.png";
	UAM_ASSERT(uam::io::WriteBinaryFile(readable, Png(4, 5)));
	const auto reject = [&](const MessageAttachment& invalid)
	{
		UAM_ASSERT(uam::attachment_preview::Read(temp.root, invalid, true, chat.id).png.empty());
		UAM_ASSERT(uam::attachment_preview::Read(temp.root, invalid, false, chat.id).png.empty());
	};
	auto invalid = attachment; invalid.path = uam::paths::Utf8PathString(readable); reject(invalid);
	invalid = attachment; invalid.preview_origin_path = uam::paths::Utf8PathString(readable); reject(invalid);
	invalid = attachment; invalid.source_uri = "file://" + uam::paths::Utf8PathString(readable); reject(invalid);
	invalid = attachment; invalid.copied = true; reject(invalid);
	invalid = attachment; invalid.native_part_key.clear(); reject(invalid);
	invalid = attachment; invalid.id.assign(16385, 'i'); reject(invalid);
	invalid = attachment; invalid.native_asset_key = "../outside"; reject(invalid);
	invalid = attachment; ++invalid.size_bytes; reject(invalid);
	invalid = attachment; invalid.size_bytes = uam::attachment_preview::kMaxSourceBytes + 1; reject(invalid);
	invalid = attachment; invalid.content_digest.front() = invalid.content_digest.front() == '0' ? '1' : '0'; reject(invalid);
	invalid = attachment; invalid.mime_type = "image/jpeg"; reject(invalid);
	invalid = attachment; invalid.mime_type = "image/gif"; reject(invalid);
	invalid = attachment; invalid.native_inline_payload = PreviewPendingOriginal({Png(2, 3), "image/png"}, "pending", "local").native_inline_payload; reject(invalid);
	AssertBoundedOwnedPreview(uam::attachment_preview::Read(temp.root, attachment, false, chat.id));
}

UAM_TEST(AttachmentPreviewOwnedRequiresAttemptAndDeclarationBeforeCache)
{
	uam_test::TempDir temp("owned-preview-publication-proof");
	const auto chat = SavePreviewOriginals(temp.root, {{Png(6, 4), "image/png"}});
	auto attachment = chat.messages.front().attachments.front();
	attachment.preview_key = uam::attachment_preview::Store(temp.root, Png(9, 7));
	UAM_ASSERT(uam::attachment_preview::IsKey(attachment.preview_key));
	const auto generation = PreviewOriginalFile(temp.root, attachment).parent_path();
	const auto attempt_path = generation / "attempted.receipt";
	const auto attempted = uam_test::ReadFile(attempt_path);
	UAM_ASSERT(!attempted.empty()); UAM_ASSERT(std::filesystem::remove(attempt_path));
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).png.empty());
	UAM_ASSERT(uam::io::WriteBinaryFile(attempt_path, attempted));
	AssertBoundedOwnedPreview(uam::attachment_preview::Read(temp.root, attachment, false, chat.id));
	const auto manifest_path = generation / "prepared.receipt";
	const auto manifest = uam_test::ReadFile(manifest_path);
	const auto position = manifest.find(attachment.native_asset_key + '\n');
	UAM_ASSERT(position != std::string::npos);
	auto undeclared = manifest; undeclared.erase(position, attachment.native_asset_key.size() + 1);
	UAM_ASSERT(uam::io::WriteBinaryFile(manifest_path, undeclared));
	UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).png.empty());
	UAM_ASSERT(uam::io::WriteBinaryFile(manifest_path, manifest));
	AssertBoundedOwnedPreview(uam::attachment_preview::Read(temp.root, attachment, false, chat.id));
}

UAM_TEST(AttachmentPreviewOwnedColdSnapshotRevalidatesProvenance)
{
	uam_test::TempDir temp("owned-preview-cold-provenance");
	const std::string id(16384, 'p');
	const auto chat = SavePreviewOriginals(temp.root, {{Png(6, 4), "image/png"}}, id);
	auto cold = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(cold && !cold->messages_loaded && cold->messages.empty());
	const auto captured = uam::attachment_preview::CaptureChatSnapshot(*cold, temp.root);
	const auto resolved = uam::attachment_preview::LoadPersistedAttachmentSnapshot(temp.root, captured, 0, id);
	UAM_ASSERT(resolved); UAM_ASSERT_EQ(resolved->attachment, chat.messages.front().attachments.front());
	AssertBoundedOwnedPreview(uam::attachment_preview::Read(temp.root, resolved->attachment, false, captured.chat_id));
	UAM_ASSERT(!cold->messages_loaded && cold->messages.empty());
	UAM_ASSERT(uam::attachment_preview::MatchesCurrentAttachmentSnapshot(*cold, captured, 0, id, *resolved));
	UAM_ASSERT(uam::attachment_preview::MatchesCurrentAttachmentSnapshot(chat, captured, 0, id, *resolved));
	const auto reject = [&](const MessageAttachment& changed)
	{
		auto current = chat; current.messages.front().attachments.front() = changed;
		UAM_ASSERT(!uam::attachment_preview::MatchesCurrentAttachmentSnapshot(current, captured, 0, id, *resolved));
	};
	const auto attachment = resolved->attachment;
	auto changed = attachment; changed.source = AttachmentSource::ProviderUri; reject(changed);
	changed = attachment; changed.source_uri = "provider:changed"; reject(changed);
	changed = attachment; changed.origin_execution_host_id = "different-origin"; reject(changed);
	changed = attachment; changed.native_part_key += "changed"; reject(changed);
	changed = attachment; changed.native_tool_call_id += "changed"; reject(changed);
	changed = attachment; changed.native_source_field += "changed"; reject(changed);
	changed = attachment; changed.native_asset_key += "changed"; reject(changed);
	changed = attachment; changed.content_digest.front() = changed.content_digest.front() == '0' ? '1' : '0'; reject(changed);
	changed = attachment; ++changed.size_bytes; reject(changed);
	changed = attachment; changed.preview_key = std::string(64, 'a'); reject(changed);
	auto duplicate = chat; duplicate.messages.front().attachments.push_back(attachment);
	UAM_ASSERT(!uam::attachment_preview::MatchesCurrentAttachmentSnapshot(duplicate, captured, 0, id, *resolved));
	UAM_ASSERT(!uam::attachment_preview::LoadPersistedAttachmentSnapshot(temp.root, captured, 1, id));
	UAM_ASSERT(!uam::attachment_preview::LoadPersistedAttachmentSnapshot(temp.root, captured, 0, "missing"));
	cold->persisted_messages_digest += "changed";
	UAM_ASSERT(!uam::attachment_preview::MatchesCurrentAttachmentSnapshot(*cold, captured, 0, id, *resolved));
	auto replacement = chat; replacement.messages.front().attachments.front().native_source_field += "/new-association";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, replacement));
	UAM_ASSERT(!uam::attachment_preview::LoadPersistedAttachmentSnapshot(temp.root, captured, 0, id));
}

UAM_TEST(AttachmentPreviewOwnedUnsupportedOriginalsRejectDerivedCache)
{
	uam_test::TempDir temp("owned-preview-unsupported");
	std::string gif; UAM_ASSERT(uam::base64::Decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", gif));
	const auto chat = SavePreviewOriginals(temp.root, {{"not an image", "image/png"}, {gif, "image/gif"}, {Png(3, 2), "image/jpeg"}});
	const auto key = uam::attachment_preview::Store(temp.root, Png(9, 7));
	UAM_ASSERT(uam::attachment_preview::IsKey(key));
	for (auto attachment : chat.messages.front().attachments)
	{
		UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).png.empty());
		attachment.preview_key = key;
		UAM_ASSERT(uam::attachment_preview::Read(temp.root, attachment, false, chat.id).png.empty());
	}
}
#endif


// Public owned-original byte-builder control; no provider/process authority is created here.
#include "common/provider/structured_attachments.h"
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(ClaudeSavedOwnedRetryCompressesLargeOriginalWithinRemoteWireBudget)
{
	uam_test::TempDir temp("claude-owned-retry-compression");
	const std::string original = Png(1024, 1024, true);
	constexpr std::size_t limit = 2 * 1024 * 1024 - 16 * 1024;
	UAM_ASSERT(original.size() > 2 * 1024 * 1024);
	// Existing public fixture publishes and ACKs the real originals, reloads
	// their exact saved associations and checks ReadOwned against these bytes.
	// Its provider label is DATA only; this test calls no runtime or transport.
	const ChatSession chat = SavePreviewOriginals(temp.root, {{original, "image/png"}});
	const auto& attachments = chat.messages.front().attachments;
	UAM_ASSERT_EQ(attachments.size(), std::size_t{1});
	UAM_ASSERT(attachments.front().source == AttachmentSource::OwnedNativeAsset);
	UAM_ASSERT(attachments.front().path.empty() && !attachments.front().native_inline_payload);
	UAM_ASSERT_EQ(attachments.front().origin_execution_host_id, chat.execution_host_id);
	UAM_ASSERT_EQ(attachments.front().size_bytes, original.size());
	const auto asset = PreviewOriginalFile(temp.root, attachments.front());
	const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const auto summary = AppPaths::UamChatSummaryFilePath(temp.root, chat.id);
	const std::string primary_before = uam_test::ReadFile(primary);
	const std::string summary_before = uam_test::ReadFile(summary);
	UAM_ASSERT_EQ(uam_test::ReadFile(asset), original);
	const auto owned_before = uam::native_assets::ReadOwned(temp.root, chat.id, attachments.front().native_asset_key);
	UAM_ASSERT(owned_before.ok && owned_before.bytes == original);
	const std::string prompt = "Inspect this retained original.\n\"Quoted\" text and \\ path.";
	std::string error;
	const auto message = uam::claude_input::BuildSavedImageRetry(temp.root, chat.id,
	    uam::paths::PathFromUtf8(chat.workspace_directory), prompt, attachments, error, {},
	    uam::claude_input::SavedImageSourcePolicy::OwnedImagesOnly, limit);
	UAM_ASSERT(error.empty() && !message.is_null());
	UAM_ASSERT(message.dump().size() + 1 <= limit);
	const auto& content = message.at("message").at("content");
	UAM_ASSERT_EQ(content.size(), std::size_t{2});
	UAM_ASSERT_EQ(content[0].at("type"), nlohmann::json("text"));
	UAM_ASSERT_EQ(content[0].at("text"), nlohmann::json(prompt +
	    "\nImage 1 (native image-block order): " + nlohmann::json(attachments.front().name).dump()));
	UAM_ASSERT_EQ(content[1].at("type"), nlohmann::json("image"));
	const auto& source = content[1].at("source");
	UAM_ASSERT_EQ(source.at("type"), nlohmann::json("base64"));
	std::string delivered;
	UAM_ASSERT(uam::base64::Decode(source.at("data").get<std::string>(), delivered));
	UAM_ASSERT(!delivered.empty() && delivered.size() < original.size() && delivered != original);
	const auto decoded = uam::attachment_preview::Decode(delivered);
	UAM_ASSERT(!decoded.png.empty() && decoded.width > 0 && decoded.height > 0);
	UAM_ASSERT(decoded.width <= 1024 && decoded.height <= 1024);
	UAM_ASSERT_EQ(source.at("media_type"), nlohmann::json(uam::attachment_preview::ImageMime(delivered)));
	UAM_ASSERT(!uam::attachment_preview::ImageMime(delivered).empty());
	const auto after = ChatRepository::LoadLocalChat(temp.root, chat.id, true);
	UAM_ASSERT(after && after->messages == chat.messages);
	UAM_ASSERT_EQ(after->execution_host_id, chat.execution_host_id);
	UAM_ASSERT_EQ(after->messages.front().attachments, attachments);
	const auto owned_after = uam::native_assets::ReadOwned(temp.root, chat.id, attachments.front().native_asset_key);
	UAM_ASSERT(owned_after.ok && owned_after.bytes == original);
	UAM_ASSERT_EQ(uam_test::ReadFile(asset), original);
	UAM_ASSERT_EQ(uam_test::ReadFile(primary), primary_before);
	UAM_ASSERT_EQ(uam_test::ReadFile(summary), summary_before);
}
#endif
