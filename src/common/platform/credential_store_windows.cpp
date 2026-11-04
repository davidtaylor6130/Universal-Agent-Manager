#include "common/platform/credential_store.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

namespace uam::platform
{
	StoredCredential ReadWindowsGenericCredential(std::string_view target, std::size_t limit)
	{
		// CredRead never shows UI. ponytail: ANSI target name suits the ASCII identifiers callers pass; use CredReadW for non-ASCII.
		const std::string name(target);
		PCREDENTIALA stored = nullptr;
		StoredCredential result;
		if (!CredReadA(name.c_str(), CRED_TYPE_GENERIC, 0, &stored))
		{
			result.status = GetLastError() == ERROR_NOT_FOUND ? StoredCredentialStatus::NotFound : StoredCredentialStatus::Unreadable;
			return result;
		}
		if (stored->CredentialBlob && stored->CredentialBlobSize > 0 && stored->CredentialBlobSize <= limit)
		{
			result.secret.assign(reinterpret_cast<const char*>(stored->CredentialBlob), stored->CredentialBlobSize);
			result.status = StoredCredentialStatus::Found;
		}
		if (stored->CredentialBlob) SecureZeroMemory(stored->CredentialBlob, stored->CredentialBlobSize);
		CredFree(stored);
		return result;
	}
}
