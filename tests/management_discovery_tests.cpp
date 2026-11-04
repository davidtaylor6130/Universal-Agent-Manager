#include "common/discovery/management_discovery.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

int main(int argc, char* argv[])
{
	using namespace uam::discovery;
	for (const std::string host : {"manager.local", "manager.local.", "127.0.0.1", "host-2.local"})
		if (!IsSafeDiscoveryHost(host)) return 1;
	for (const std::string host : {"", ".local", "-host.local", "host/local", "user@host", "host:443", "host..local", "host-.local", "host\n.local"})
		if (IsSafeDiscoveryHost(host)) return 1;
	if (IsSafeDiscoveryHost(std::string(254, 'x'))) return 1;
	std::string error;
	if (ManagementAnnouncement::Start("", "manager.local", 443, true, error) || error.empty()) return 1;
	if (argc > 1 && std::string(argv[1]) == "--native-local-only")
	{
#if defined(__APPLE__)
		error.clear();
		std::unique_ptr<ManagementAnnouncement> announcement = ManagementAnnouncement::Start("uam-owned-native-check", "uam-owned-native-check.local.", 58949, true, error);
		if (!announcement) { std::cerr << error << '\n'; return 1; }
		for (int index = 0; index < 10; ++index)
		{
			if (!announcement->Poll().empty()) return 1;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		const std::vector<ManagementHint> hints = BrowseManagementNodes(true, error);
		bool found = false;
		for (const ManagementHint& hint : hints)
			if (hint.host == "uam-owned-native-check.local." && hint.port == 58949) found = true;
		if (!found || !error.empty()) { std::cerr << "Native local-only hint not resolved: " << error << '\n'; return 1; }
		announcement.reset();
		for (const ManagementHint& hint : BrowseManagementNodes(true, error))
			if (hint.host == "uam-owned-native-check.local." && hint.port == 58949) return 1;
#else
		std::cerr << "Local-only DNS-SD integration is macOS only.\n";
		return 1;
#endif
	}
	std::cout << "PASS native management discovery validation and requested local-only lifecycle checks\n";
}
