#include "cef/ui_asset_snapshot.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
void Check(bool condition, const char* message)
{
	if (!condition) throw std::runtime_error(message);
}
void Write(const fs::path& path, const std::string& bytes)
{
	fs::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << bytes;
}
int main()
{
	const fs::path root = fs::temp_directory_path() / ("uam-ui-snapshot-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	try
	{
		Write(root / "index.html", "old index assets/old-modal.js");
		Write(root / ".vite/manifest.json", R"({"index.html":{"file":"assets/old-modal.js"}})");
		Write(root / "assets/old-modal.js", "old modal");
		const uam::cef::UiAssetSnapshot old(root);
		fs::remove_all(root);
		Write(root / "index.html", "new index assets/new-modal.js");
		Write(root / ".vite/manifest.json", R"({"index.html":{"file":"assets/new-modal.js"}})");
		Write(root / "assets/new-modal.js", "new modal");
		const uam::cef::UiAssetSnapshot current(root);
		Check(old.Find("assets/old-modal.js") && *old.Find("assets/old-modal.js") == "old modal", "Live snapshot lost a replaced lazy chunk.");
		Check(*old.Find("index.html") == "old index assets/old-modal.js", "Reload crossed into a different build.");
		Check(!old.Find("assets/new-modal.js") && !old.Find("../index.html"), "Snapshot mixed builds or accepted traversal.");
		Check(!current.Find("assets/old-modal.js") && *current.Find("index.html") == "new index assets/new-modal.js", "Fresh instance did not use the new build.");
		Write(root / "empty.txt", "");
		Check(uam::cef::UiAssetSnapshot(root).Find("empty.txt") != nullptr, "Empty resources were rejected.");
		fs::remove(root / "index.html");
		bool failed = false;
		try { const uam::cef::UiAssetSnapshot missing(root); } catch (const std::exception&) { failed = true; }
		Check(failed, "Missing startup document was not rejected.");
		Write(root / "index.html", "new index assets/new-modal.js");
		std::error_code error;
		fs::create_symlink(root / "index.html", root / "linked.html", error);
		if (!error)
		{
			failed = false;
			try { const uam::cef::UiAssetSnapshot linked(root); } catch (const std::exception&) { failed = true; }
			Check(failed, "Linked resource was accepted.");
			fs::remove(root / "linked.html");
		}
		fs::remove(root / "assets/new-modal.js");
		failed = false;
		try { const uam::cef::UiAssetSnapshot incomplete(root); } catch (const std::exception&) { failed = true; }
		Check(failed, "Missing manifest chunk was accepted.");
		Write(root / "assets/new-modal.js", "new modal");
		Write(root / "oversized.bin", std::string(uam::cef::UiAssetSnapshot::kMaxFileBytes + 1, 'x'));
		failed = false;
		try { const uam::cef::UiAssetSnapshot oversized(root); } catch (const std::exception&) { failed = true; }
		Check(failed, "Oversized resource was accepted.");
		uam::cef::UiRecoveryBudget budget;
		for (int i = 0; i < 3; ++i) Check(budget.AllowAutomaticRecovery(), "Recovery stopped too early.");
		for (int i = 0; i < 100; ++i) Check(!budget.AllowAutomaticRecovery(), "Recovery exceeded its limit.");
		budget.Reset();
		Check(budget.AllowAutomaticRecovery(), "Manual recovery did not reset the retry budget.");
		fs::remove_all(root);
		std::cout << "UI snapshot replacement, limits, missing index, symlink, and recovery budget passed.\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		fs::remove_all(root);
		std::cerr << error.what() << '\n';
		return 1;
	}
}
