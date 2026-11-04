#pragma once
#include <memory>
#include <string>
#include <vector>

namespace uam::discovery
{
struct ManagementHint
{
	std::string name;
	std::string host;
	unsigned short port = 0;
};
/// <summary>Untrusted DNS-SD address hints. Discovery never connects or enrolls a peer.</summary>
std::vector<ManagementHint> BrowseManagementNodes(bool local_only, std::string& error);
/// <summary>Announce an explicitly configured HTTPS endpoint using the native OS responder.</summary>
class ManagementAnnouncement
{
public:
	static std::unique_ptr<ManagementAnnouncement> Start(const std::string& name, const std::string& host, unsigned short port, bool local_only, std::string& error);
	~ManagementAnnouncement();
	std::string Poll();
private:
	struct Impl;
	explicit ManagementAnnouncement(std::unique_ptr<Impl> implementation);
	std::unique_ptr<Impl> m_implementation;
};
bool IsSafeDiscoveryHost(const std::string& host);
}
