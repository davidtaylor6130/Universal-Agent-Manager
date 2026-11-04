#include "management_discovery.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>
#if defined(__APPLE__)
#include <dns_sd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#elif defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#include <winsock2.h>
#include <windows.h>
#include <windns.h>
#endif

namespace uam::discovery
{
bool IsSafeDiscoveryHost(const std::string& host)
{
	if (host.empty() || host.size() > 253 || host.front() == '.' || host.front() == '-') return false;
	std::size_t label_start = 0;
	for (std::size_t index = 0; index <= host.size(); ++index)
	{
		if (index < host.size() && host[index] != '.') continue;
		const std::size_t label_length = index - label_start;
		if (label_length == 0 && index == host.size() && index > 0 && host.back() == '.') break;
		if (label_length == 0 || label_length > 63 || host[label_start] == '-' || host[index - 1] == '-') return false;
		label_start = index + 1;
	}
	return std::all_of(host.begin(), host.end(), [](unsigned char character)
	{
		return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
		       (character >= '0' && character <= '9') || character == '-' || character == '.';
	});
}
#if defined(__APPLE__)
namespace
{
struct BrowseState
{
	bool local_only = false;
	std::vector<ManagementHint> hints;
	std::vector<DNSServiceRef> resolutions;
};
void Resolved(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType error, const char* name, const char* host, uint16_t port, uint16_t, const unsigned char*, void* context)
{
	BrowseState& state = *static_cast<BrowseState*>(context);
	if (error != kDNSServiceErr_NoError || !host || !name || !IsSafeDiscoveryHost(host) || !ntohs(port) || state.hints.size() >= 32) return;
	const ManagementHint hint{std::string(name).substr(0, 253), host, ntohs(port)};
	if (std::none_of(state.hints.begin(), state.hints.end(), [&hint](const ManagementHint& existing) { return existing.host == hint.host && existing.port == hint.port; })) state.hints.push_back(hint);
}
void Browsed(DNSServiceRef, DNSServiceFlags flags, uint32_t interface_index, DNSServiceErrorType error, const char* name, const char* type, const char* domain, void* context)
{
	BrowseState& state = *static_cast<BrowseState*>(context);
	if (error || !(flags & kDNSServiceFlagsAdd) || state.resolutions.size() >= 32) return;
	DNSServiceRef resolution = nullptr;
	if (DNSServiceResolve(&resolution, 0, state.local_only ? kDNSServiceInterfaceIndexLocalOnly : interface_index, name, type, domain, Resolved, context) == kDNSServiceErr_NoError) state.resolutions.push_back(resolution);
}
DNSServiceErrorType ProcessReady(const std::vector<DNSServiceRef>& services, long microseconds)
{
	fd_set readable;
	FD_ZERO(&readable);
	int maximum = -1;
	for (DNSServiceRef service : services)
	{
		const int descriptor = DNSServiceRefSockFD(service);
		if (descriptor < 0 || descriptor >= FD_SETSIZE) continue;
		FD_SET(descriptor, &readable);
		maximum = std::max(maximum, descriptor);
	}
	timeval timeout{0, static_cast<decltype(timeval::tv_usec)>(microseconds)};
	if (maximum < 0 || select(maximum + 1, &readable, nullptr, nullptr, &timeout) <= 0) return kDNSServiceErr_NoError;
	for (DNSServiceRef service : services)
	{
		const int descriptor = DNSServiceRefSockFD(service);
		if (descriptor >= 0 && descriptor < FD_SETSIZE && FD_ISSET(descriptor, &readable))
		{
			const DNSServiceErrorType status = DNSServiceProcessResult(service);
			if (status) return status;
		}
	}
	return kDNSServiceErr_NoError;
}
void Registered(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType error, const char*, const char*, const char*, void* context)
{
	static_cast<std::atomic<int>*>(context)->store(error);
}
}
struct ManagementAnnouncement::Impl
{
	DNSServiceRef service = nullptr;
	std::atomic<int> status{0};
};
std::vector<ManagementHint> BrowseManagementNodes(bool local_only, std::string& error)
{
	BrowseState state;
	state.local_only = local_only;
	DNSServiceRef browse = nullptr;
	const DNSServiceErrorType status = DNSServiceBrowse(&browse, 0, local_only ? kDNSServiceInterfaceIndexLocalOnly : 0, "_uam-manager._tcp", "local.", Browsed, &state);
	if (status)
	{
		error = "Native DNS-SD browse failed: " + std::to_string(status);
		return {};
	}
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < deadline)
	{
		std::vector<DNSServiceRef> services = state.resolutions;
		services.push_back(browse);
		const DNSServiceErrorType processed = ProcessReady(services, 100000);
		if (processed)
		{
			error = "Native DNS-SD browse failed: " + std::to_string(processed);
			break;
		}
	}
	for (DNSServiceRef resolution : state.resolutions) DNSServiceRefDeallocate(resolution);
	DNSServiceRefDeallocate(browse);
	return state.hints;
}
std::unique_ptr<ManagementAnnouncement> ManagementAnnouncement::Start(const std::string& name, const std::string& host, unsigned short port, bool local_only, std::string& error)
{
	if (name.empty() || name.size() > 63 || !IsSafeDiscoveryHost(name) || name.find('.') != std::string::npos || !IsSafeDiscoveryHost(host) || !port)
	{
		error = "Invalid discovery name, host or port.";
		return {};
	}
	std::unique_ptr<Impl> implementation = std::make_unique<Impl>();
	const DNSServiceErrorType status = DNSServiceRegister(&implementation->service, kDNSServiceFlagsNoAutoRename, local_only ? kDNSServiceInterfaceIndexLocalOnly : 0, name.c_str(), "_uam-manager._tcp", "local.", host.c_str(), htons(port), 0, nullptr, Registered, &implementation->status);
	if (status)
	{
		error = "Native DNS-SD announcement failed: " + std::to_string(status);
		return {};
	}
	return std::unique_ptr<ManagementAnnouncement>(new ManagementAnnouncement(std::move(implementation)));
}
ManagementAnnouncement::~ManagementAnnouncement()
{
	if (m_implementation->service) DNSServiceRefDeallocate(m_implementation->service);
}
std::string ManagementAnnouncement::Poll()
{
	const DNSServiceErrorType processed = ProcessReady({m_implementation->service}, 0);
	if (processed) m_implementation->status.store(processed);
	const int status = m_implementation->status.load();
	return status ? "Native DNS-SD announcement failed: " + std::to_string(status) : std::string{};
}
#elif defined(_WIN32)
namespace
{
std::string Utf8(PCWSTR value)
{
	if (!value) return
	{
	};
	const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
	if (length < 1 || length > 1024) return
	{
	};
	std::string text(static_cast<std::size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), length, nullptr, nullptr);
	text.pop_back();
	return text;
}
std::wstring Wide(const std::string& value)
{
	const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	std::wstring text(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
	if (length > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), text.data(), length);
	return text;
}
struct WindowsBrowse
{
	std::mutex mutex;
	std::vector<ManagementHint> hints;
	DNS_SERVICE_BROWSE_REQUEST request{};
	DNS_SERVICE_CANCEL cancel{};
	HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	// Native cancellation is asynchronous. Retain the callback/request until its final acknowledgement.
	std::shared_ptr<WindowsBrowse> lifetime;
	~WindowsBrowse()
	{
		if (completed) CloseHandle(completed);
	}
};
void WINAPI BrowsedWindows(DWORD status, PVOID context, PDNS_RECORD records)
{
	WindowsBrowse& state = *static_cast<WindowsBrowse*>(context);
	if (status == ERROR_SUCCESS)
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		for (PDNS_RECORD record = records; record && state.hints.size() < 32; record = record->pNext)
		{
			if (record->wType != DNS_TYPE_SRV) continue;
			const std::string host = Utf8(record->Data.SRV.pNameTarget);
			const std::string name = Utf8(record->pName);
			if (!IsSafeDiscoveryHost(host) || name.empty() || !record->Data.SRV.wPort) continue;
			const ManagementHint hint{name.substr(0, 253), host, record->Data.SRV.wPort};
			if (std::none_of(state.hints.begin(), state.hints.end(), [&hint](const ManagementHint& existing) { return existing.host == hint.host && existing.port == hint.port; })) state.hints.push_back(hint);
		}
	}
	if (records) DnsRecordListFree(records, DnsFreeRecordList);
	if (status == ERROR_CANCELLED)
	{
		SetEvent(state.completed);
		state.lifetime.reset();
	}
}
struct WindowsRegistration
{
	std::atomic<DWORD> status{ERROR_SUCCESS};
	DNS_SERVICE_REGISTER_REQUEST request{};
	DNS_SERVICE_CANCEL cancel{};
	PDNS_SERVICE_INSTANCE instance = nullptr;
	HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	std::shared_ptr<WindowsRegistration> lifetime;
	~WindowsRegistration()
	{
		if (instance) DnsServiceFreeInstance(instance);
		if (completed) CloseHandle(completed);
	}
};
void WINAPI RegisteredWindows(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
{
	WindowsRegistration& state = *static_cast<WindowsRegistration*>(context);
	state.status.store(status);
	if (instance) DnsServiceFreeInstance(instance);
	SetEvent(state.completed);
}
}
struct ManagementAnnouncement::Impl
{
	std::shared_ptr<WindowsRegistration> state;
};
std::vector<ManagementHint> BrowseManagementNodes(bool local_only, std::string& error)
{
	if (local_only)
	{
		error = "Windows native DNS-SD has no local-only interface mode; no network query was started.";
		return {};
	}
	std::shared_ptr<WindowsBrowse> state = std::make_shared<WindowsBrowse>();
	if (!state->completed)
	{
		error = "Cannot create native discovery cancellation event.";
		return {};
	}
	state->request.Version = DNS_QUERY_REQUEST_VERSION1;
	state->request.QueryName = L"_uam-manager._tcp.local";
	state->request.pBrowseCallback = BrowsedWindows;
	state->request.pQueryContext = state.get();
	state->lifetime = state;
	const DNS_STATUS status = DnsServiceBrowse(&state->request, &state->cancel);
	if (status != DNS_REQUEST_PENDING)
	{
		state->lifetime.reset();
		error = "Native DNS-SD browse failed: " + std::to_string(status);
		return {};
	}
	std::this_thread::sleep_for(std::chrono::seconds(5));
	const DNS_STATUS cancelled = DnsServiceBrowseCancel(&state->cancel);
	if (cancelled != ERROR_SUCCESS || WaitForSingleObject(state->completed, 2000) != WAIT_OBJECT_0) error = "Native DNS-SD cancellation was not acknowledged.";
	std::lock_guard<std::mutex> lock(state->mutex);
	return state->hints;
}
std::unique_ptr<ManagementAnnouncement> ManagementAnnouncement::Start(const std::string& name, const std::string& host, unsigned short port, bool local_only, std::string& error)
{
	if (local_only)
	{
		error = "Windows native DNS-SD has no local-only announcement mode; nothing was advertised.";
		return {};
	}
	if (name.empty() || name.size() > 63 || !IsSafeDiscoveryHost(name) || name.find('.') != std::string::npos || !IsSafeDiscoveryHost(host) || !port)
	{
		error = "Invalid discovery name, host or port.";
		return {};
	}
	std::shared_ptr<WindowsRegistration> state = std::make_shared<WindowsRegistration>();
	const std::wstring instance_name = Wide(name + "._uam-manager._tcp.local");
	const std::wstring host_name = Wide(host);
	state->instance = DnsServiceConstructInstance(instance_name.c_str(), host_name.c_str(), nullptr, nullptr, port, 0, 0, 0, nullptr, nullptr);
	if (!state->instance || !state->completed)
	{
		error = "Cannot create native DNS-SD announcement.";
		return {};
	}
	state->request.Version = DNS_QUERY_REQUEST_VERSION1;
	state->request.pServiceInstance = state->instance;
	state->request.pRegisterCompletionCallback = RegisteredWindows;
	state->request.pQueryContext = state.get();
	state->lifetime = state;
	const DWORD status = DnsServiceRegister(&state->request, &state->cancel);
	if (status != DNS_REQUEST_PENDING)
	{
		state->lifetime.reset();
		error = "Native DNS-SD announcement failed: " + std::to_string(status);
		return {};
	}
	std::unique_ptr<Impl> implementation = std::make_unique<Impl>();
	implementation->state = state;
	return std::unique_ptr<ManagementAnnouncement>(new ManagementAnnouncement(std::move(implementation)));
}
ManagementAnnouncement::~ManagementAnnouncement()
{
	std::shared_ptr<WindowsRegistration> state = m_implementation->state;
	if (WaitForSingleObject(state->completed, 0) != WAIT_OBJECT_0)
	{
		(void)DnsServiceRegisterCancel(&state->cancel);
		if (WaitForSingleObject(state->completed, 2000) != WAIT_OBJECT_0)
		{
			// ponytail: retain one pending native callback until process exit if cancellation is unacknowledged.
			// Never reuse its completion event for deregistration while registration may still complete.
			return;
		}
	}
	if (state->status.load() != ERROR_SUCCESS)
	{
		state->lifetime.reset();
		return;
	}
	ResetEvent(state->completed);
	const DWORD deregistered = DnsServiceDeRegister(&state->request, nullptr);
	if (deregistered != DNS_REQUEST_PENDING || WaitForSingleObject(state->completed, 2000) == WAIT_OBJECT_0)
	{
		state->lifetime.reset();
	}
}
std::string ManagementAnnouncement::Poll()
{
	const DWORD status = m_implementation->state->status.load();
	return status ? "Native DNS-SD announcement failed: " + std::to_string(status) : std::string{};
}
#else
struct ManagementAnnouncement::Impl
{
};
std::vector<ManagementHint> BrowseManagementNodes(bool, std::string& error)
{
	error = "Native management discovery is unavailable on this build.";
	return {};
}
std::unique_ptr<ManagementAnnouncement> ManagementAnnouncement::Start(const std::string&, const std::string&, unsigned short, bool, std::string& error)
{
	error = "Native management discovery is unavailable on this build.";
	return {};
}
ManagementAnnouncement::~ManagementAnnouncement() = default;
std::string ManagementAnnouncement::Poll() { return {}; }
#endif
ManagementAnnouncement::ManagementAnnouncement(std::unique_ptr<Impl> implementation) : m_implementation(std::move(implementation))
{
}
}
