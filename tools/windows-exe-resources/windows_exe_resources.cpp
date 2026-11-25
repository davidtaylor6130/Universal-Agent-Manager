// Build tool for the Windows CEF sandbox layout: universal_agent_manager.exe is CEF's bootstrap.exe, renamed.
// Usage: uam_windows_exe_resources <universal_agent_manager.dll> <universal_agent_manager.exe>
// Replaces the bootstrap's icons with the DLL's IDI_APP_ICON group (from src/uam.rc) and removes the bootstrap's
// version resource, so Explorer and Task Manager show UAM as they did before the layout changed. A signed bootstrap
// is refused: editing its resources would invalidate the signature, which bootstrap.exe itself verifies.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	constexpr const wchar_t* kIconGroupName = L"IDI_APP_ICON";
	// Wide resource types, independent of whether UNICODE is defined for this tool.
	const LPCWSTR kIconImage = MAKEINTRESOURCEW(3);
	const LPCWSTR kIconGroup = MAKEINTRESOURCEW(14);
	const LPCWSTR kVersion = MAKEINTRESOURCEW(16);

	struct ResourceName
	{
		std::wstring text;
		WORD id = 0;

		LPCWSTR Get() const
		{
			return text.empty() ? MAKEINTRESOURCEW(id) : text.c_str();
		}
	};

	struct ResourceEntry
	{
		LPCWSTR type = nullptr;
		ResourceName name;
		WORD language = 0;
		std::vector<unsigned char> data;
	};

#pragma pack(push, 2)
	struct GroupIconEntry
	{
		BYTE width;
		BYTE height;
		BYTE color_count;
		BYTE reserved;
		WORD planes;
		WORD bit_count;
		DWORD bytes_in_resource;
		WORD id;
	};
#pragma pack(pop)

	ResourceName NameOf(LPCWSTR name)
	{
		ResourceName result;
		if (IS_INTRESOURCE(name))
			result.id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name));
		else
			result.text = name;
		return result;
	}

	struct Enumeration
	{
		LPCWSTR type = nullptr;
		std::vector<ResourceEntry>* entries = nullptr;
		bool read_data = false;
	};

	BOOL CALLBACK OnLanguage(HMODULE module, LPCWSTR type, LPCWSTR name, WORD language, LONG_PTR parameter)
	{
		auto* enumeration = reinterpret_cast<Enumeration*>(parameter);
		ResourceEntry entry;
		entry.type = enumeration->type;
		entry.name = NameOf(name);
		entry.language = language;
		if (enumeration->read_data)
		{
			HRSRC found = FindResourceExW(module, type, name, language);
			HGLOBAL loaded = found != nullptr ? LoadResource(module, found) : nullptr;
			const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
			const DWORD size = found != nullptr ? SizeofResource(module, found) : 0;
			if (bytes == nullptr || size == 0) return FALSE;
			entry.data.assign(static_cast<const unsigned char*>(bytes), static_cast<const unsigned char*>(bytes) + size);
		}
		enumeration->entries->push_back(std::move(entry));
		return TRUE;
	}

	BOOL CALLBACK OnName(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR parameter)
	{
		EnumResourceLanguagesW(module, type, name, OnLanguage, parameter);
		return TRUE;
	}

	/// <summary>Lists every resource of one type (all names and languages), optionally with its bytes.</summary>
	std::vector<ResourceEntry> List(HMODULE module, LPCWSTR type, bool read_data)
	{
		std::vector<ResourceEntry> entries;
		Enumeration enumeration{type, &entries, read_data};
		EnumResourceNamesW(module, type, OnName, reinterpret_cast<LONG_PTR>(&enumeration));
		return entries;
	}

	bool HasAuthenticodeSignature(const wchar_t* path)
	{
		std::ifstream file(path, std::ios::binary);
		IMAGE_DOS_HEADER dos{};
		if (!file.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return true;
		IMAGE_NT_HEADERS64 headers{};
		if (!file.seekg(dos.e_lfanew) || !file.read(reinterpret_cast<char*>(&headers), sizeof(headers)) || headers.Signature != IMAGE_NT_SIGNATURE) return true;
		if (headers.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return true;
		return headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].Size != 0;
	}

	int Fail(const char* message)
	{
		std::fprintf(stderr, "uam_windows_exe_resources: %s (error %lu)\n", message, GetLastError());
		return 1;
	}
}

int wmain(int argc, wchar_t** argv)
{
	if (argc != 3)
	{
		std::fprintf(stderr, "Usage: uam_windows_exe_resources <source.dll> <target.exe>\n");
		return 2;
	}
	const wchar_t* source_path = argv[1];
	const wchar_t* target_path = argv[2];
	if (HasAuthenticodeSignature(target_path)) return Fail("the target is signed or is not a 64-bit executable; sign after editing resources");

	// Collect the source icon group and the icon images it names.
	HMODULE source = LoadLibraryExW(source_path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	if (source == nullptr) return Fail("cannot open the source module");
	std::vector<ResourceEntry> additions;
	for (ResourceEntry& group : List(source, kIconGroup, true))
	{
		if (group.name.text != kIconGroupName) continue;
		if (group.data.size() < 6) return Fail("the source icon group is malformed");
		const WORD count = *reinterpret_cast<const WORD*>(group.data.data() + 4);
		if (group.data.size() < 6 + static_cast<std::size_t>(count) * sizeof(GroupIconEntry)) return Fail("the source icon group is truncated");
		for (WORD index = 0; index < count; ++index)
		{
			const auto* icon = reinterpret_cast<const GroupIconEntry*>(group.data.data() + 6 + index * sizeof(GroupIconEntry));
			HRSRC found = FindResourceExW(source, kIconImage, MAKEINTRESOURCEW(icon->id), group.language);
			HGLOBAL loaded = found != nullptr ? LoadResource(source, found) : nullptr;
			const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
			const DWORD size = found != nullptr ? SizeofResource(source, found) : 0;
			if (bytes == nullptr || size == 0) return Fail("the source icon group names a missing icon image");
			ResourceEntry image;
			image.type = kIconImage;
			image.name.id = icon->id;
			image.language = group.language;
			image.data.assign(static_cast<const unsigned char*>(bytes), static_cast<const unsigned char*>(bytes) + size);
			additions.push_back(std::move(image));
		}
		additions.push_back(std::move(group));
	}
	FreeLibrary(source);
	if (additions.empty()) return Fail("the source module has no IDI_APP_ICON icon group");

	// Collect what the bootstrap carries that must go: its icons and its version text.
	HMODULE target = LoadLibraryExW(target_path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	if (target == nullptr) return Fail("cannot open the target executable");
	std::vector<ResourceEntry> removals;
	for (LPCWSTR type : {kIconGroup, kIconImage, kVersion})
	{
		std::vector<ResourceEntry> entries = List(target, type, false);
		removals.insert(removals.end(), entries.begin(), entries.end());
	}
	FreeLibrary(target);

	HANDLE update = BeginUpdateResourceW(target_path, FALSE);
	if (update == nullptr) return Fail("cannot start updating the target resources");
	for (const ResourceEntry& entry : removals)
	{
		if (!UpdateResourceW(update, entry.type, entry.name.Get(), entry.language, nullptr, 0))
		{
			EndUpdateResourceW(update, TRUE);
			return Fail("cannot remove a bootstrap resource");
		}
	}
	for (ResourceEntry& entry : additions)
	{
		if (!UpdateResourceW(update, entry.type, entry.name.Get(), entry.language, entry.data.data(), static_cast<DWORD>(entry.data.size())))
		{
			EndUpdateResourceW(update, TRUE);
			return Fail("cannot add UAM's icon");
		}
	}
	if (!EndUpdateResourceW(update, FALSE)) return Fail("cannot write the target resources");
	return 0;
}
