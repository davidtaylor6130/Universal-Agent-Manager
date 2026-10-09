#include "app/attachment_preview_service.h"
#include <windows.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

namespace uam::attachment_preview
{
	namespace
	{
		template<class T> struct Release { void operator()(T* value) const { if (value) value->Release(); } };
		template<class T> using ComPtr = std::unique_ptr<T, Release<T>>;

		class BoundedStream final : public IStream
		{
		public:
			explicit BoundedStream(std::size_t limit, std::stop_token cancel) : m_limit(limit), m_cancel(cancel) {}
			std::string bytes;
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override
			{
				if (!result) return E_POINTER;
				*result = nullptr;
				if (id != IID_IUnknown && id != IID_ISequentialStream && id != IID_IStream) return E_NOINTERFACE;
				*result = static_cast<IStream*>(this); AddRef(); return S_OK;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
			ULONG STDMETHODCALLTYPE Release() override { const ULONG count = --m_refs; if (!count) delete this; return count; }
			HRESULT STDMETHODCALLTYPE Read(void* target, ULONG count, ULONG* read) override
			{
				if (!target && count) return E_POINTER;
				const std::size_t size = std::min<std::size_t>(count, bytes.size() - std::min(m_offset, bytes.size()));
				if (size) std::memcpy(target, bytes.data() + m_offset, size);
				m_offset += size; if (read) *read = static_cast<ULONG>(size); return size == count ? S_OK : S_FALSE;
			}
			HRESULT STDMETHODCALLTYPE Write(const void* source, ULONG count, ULONG* written) override
			{
				if (written) *written = 0;
				if (m_cancel.stop_requested()) return STG_E_REVERTED;
				if ((!source && count) || count > m_limit - m_offset) return STG_E_MEDIUMFULL;
				bytes.resize(std::max(bytes.size(), m_offset + count));
				if (count) std::memcpy(bytes.data() + m_offset, source, count);
				m_offset += count; if (written) *written = count; return S_OK;
			}
			HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER delta, DWORD origin, ULARGE_INTEGER* result) override
			{
				const LONGLONG base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? static_cast<LONGLONG>(m_offset) :
				    origin == STREAM_SEEK_END ? static_cast<LONGLONG>(bytes.size()) : -1;
				if (base < 0 || delta.QuadPart < -base || delta.QuadPart > static_cast<LONGLONG>(m_limit) - base) return STG_E_INVALIDFUNCTION;
				m_offset = static_cast<std::size_t>(base + delta.QuadPart);
				if (result) result->QuadPart = m_offset; return S_OK;
			}
			HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override { if (size.QuadPart > m_limit) return STG_E_MEDIUMFULL; bytes.resize(static_cast<std::size_t>(size.QuadPart)); return S_OK; }
			HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE Stat(STATSTG* info, DWORD) override { if (!info) return E_POINTER; *info = {}; info->type = STGTY_STREAM; info->cbSize.QuadPart = bytes.size(); return S_OK; }
			HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
		private:
			std::atomic<ULONG> m_refs{1};
			std::size_t m_offset = 0;
			const std::size_t m_limit;
			const std::stop_token m_cancel;
		};
	}

	std::string Digest(std::string_view bytes)
	{
		HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
		if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return {};
		unsigned char digest[32]{}; DWORD size = sizeof(digest);
		const bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
		    CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0) &&
		    CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) && size == 32;
		if (hash) CryptDestroyHash(hash);
		CryptReleaseContext(provider, 0);
		if (!ok) return {};
		std::string result; result.reserve(64);
		for (unsigned char value : digest) { result += "0123456789abcdef"[value >> 4]; result += "0123456789abcdef"[value & 15]; }
		return result;
	}

	static DeliveryImage ProcessImage(std::string_view source, std::size_t max_bytes, bool delivery, std::stop_token cancel)
	{
		if (cancel.stop_requested() || source.empty() || source.size() > kMaxSourceBytes || max_bytes == 0 || max_bytes > kMaxSourceBytes) return {};
		const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return {};
		struct Apartment { bool owns; ~Apartment() { if (owns) CoUninitialize(); } } apartment{SUCCEEDED(initialized)};
		IWICImagingFactory* raw_factory = nullptr;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw_factory)))) return {};
		ComPtr<IWICImagingFactory> factory(raw_factory);
		IWICStream* raw_stream = nullptr;
		if (FAILED(factory->CreateStream(&raw_stream))) return {};
		ComPtr<IWICStream> stream(raw_stream);
		if (FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(source.data())), static_cast<DWORD>(source.size())))) return {};
		IWICBitmapDecoder* raw_decoder = nullptr;
		if (FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, &raw_decoder))) return {};
		ComPtr<IWICBitmapDecoder> decoder(raw_decoder);
		const std::string mime = ImageMime(source);
		if (mime.empty()) return {};
		GUID container{};
		if (FAILED(decoder->GetContainerFormat(&container)) || (!delivery && container != GUID_ContainerFormatPng && container != GUID_ContainerFormatJpeg)) return {};
		IWICBitmapFrameDecode* raw_frame = nullptr;
		if (FAILED(decoder->GetFrame(0, &raw_frame))) return {};
		ComPtr<IWICBitmapFrameDecode> frame(raw_frame);
		UINT width = 0, height = 0;
		if (FAILED(frame->GetSize(&width, &height)) || !width || !height || width > kMaxSourceSide || height > kMaxSourceSide ||
		    static_cast<std::uint64_t>(width) * height > kMaxSourcePixels) return {};
		const bool original_fits = delivery && source.size() <= max_bytes && width <= 8000 && height <= 8000;
		UINT frames = 0;
		if (FAILED(decoder->GetFrameCount(&frames))) return {};
		const bool animated = frames > 1;
		if (delivery && !original_fits && animated) return {};
		unsigned orientation = 1;
		IWICMetadataQueryReader* raw_metadata = nullptr;
		if (SUCCEEDED(frame->GetMetadataQueryReader(&raw_metadata)))
		{
			ComPtr<IWICMetadataQueryReader> metadata(raw_metadata);
			PROPVARIANT value{};
			if (SUCCEEDED(metadata->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value)) && value.vt == VT_UI2) orientation = value.uiVal;
			PropVariantClear(&value);
		}
		const WICBitmapTransformOptions transforms[] = {WICBitmapTransformRotate0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal,
		    WICBitmapTransformRotate180, WICBitmapTransformFlipVertical, static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
		    WICBitmapTransformRotate90, static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal), WICBitmapTransformRotate270};
		IWICBitmapFlipRotator* raw_rotator = nullptr;
		if (FAILED(factory->CreateBitmapFlipRotator(&raw_rotator))) return {};
		ComPtr<IWICBitmapFlipRotator> rotator(raw_rotator);
		if (FAILED(rotator->Initialize(frame.get(), transforms[orientation <= 8 ? orientation : 1])) || FAILED(rotator->GetSize(&width, &height))) return {};
		const std::vector<UINT> sides = delivery ? std::vector<UINT>{8000, 4096, 2576, 2048, 1568, 1024, 768, 512, 256} : std::vector<UINT>{1024, 512, 256};
		for (UINT side : sides)
		{
			if (cancel.stop_requested()) return {};
			if (original_fits) side = 1024;
			const UINT largest = std::max(width, height);
			const UINT out_width = largest <= side ? width : std::max(1u, static_cast<UINT>(static_cast<std::uint64_t>(width) * side / largest));
			const UINT out_height = largest <= side ? height : std::max(1u, static_cast<UINT>(static_cast<std::uint64_t>(height) * side / largest));
			IWICBitmapScaler* raw_scaler = nullptr;
			if (FAILED(factory->CreateBitmapScaler(&raw_scaler))) return {};
			ComPtr<IWICBitmapScaler> scaler(raw_scaler);
			if (FAILED(scaler->Initialize(rotator.get(), out_width, out_height, WICBitmapInterpolationModeFant))) return {};
			ComPtr<BoundedStream> output(new BoundedStream(max_bytes, cancel));
			IWICBitmapEncoder* raw_encoder = nullptr;
			if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &raw_encoder))) return {};
			ComPtr<IWICBitmapEncoder> encoder(raw_encoder);
			if (FAILED(encoder->Initialize(output.get(), WICBitmapEncoderNoCache))) return {};
			IWICBitmapFrameEncode* raw_encoded = nullptr;
			if (FAILED(encoder->CreateNewFrame(&raw_encoded, nullptr))) return {};
			ComPtr<IWICBitmapFrameEncode> encoded(raw_encoded);
			WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
			if (FAILED(encoded->Initialize(nullptr)) || FAILED(encoded->SetSize(out_width, out_height)) || FAILED(encoded->SetPixelFormat(&format))) return {};
			IWICFormatConverter* raw_converter = nullptr;
			if (FAILED(factory->CreateFormatConverter(&raw_converter))) return {};
			ComPtr<IWICFormatConverter> converter(raw_converter);
			if (FAILED(converter->Initialize(scaler.get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
			if (original_fits)
			{
				// Force bounded pixel decoding before admitting the unchanged original bytes.
				const UINT stride = out_width * 4;
				std::vector<BYTE> pixels(static_cast<std::size_t>(stride) * out_height);
				if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())) || cancel.stop_requested()) return {};
				return {std::string(source), width, height, mime, animated};
			}
			if (SUCCEEDED(encoded->WriteSource(converter.get(), nullptr)) && SUCCEEDED(encoded->Commit()) && SUCCEEDED(encoder->Commit()) && !output->bytes.empty())
				return {std::move(output->bytes), out_width, out_height, "image/png"};
		}
		return {};
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
